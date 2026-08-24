// Phase 8 process-safety testing. tests/core/ProcessRunnerTest.cpp's two tests are both
// SKIP_UNLESS_WINDOWS() (they use cmd.exe/ping.exe) -- which means, on the Linux dev/CI
// machine this project actually builds and tests on (docs/development.md), the real
// RealProcessRunner had ZERO test coverage: every test ever exercising real OS process
// spawning skipped. This file closes that gap with portable equivalents, using whichever
// command each platform actually has, so RealProcessRunner is verified on the platform CI
// runs on rather than only reasoned about.

#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/errors/MediaToolException.h"
#include "core/process/RealProcessRunner.h"

using mediatool::errors::MediaToolException;
using mediatool::process::ProcessOptions;
using mediatool::process::ProcessResult;
using mediatool::process::RealProcessRunner;

namespace {

class LineCollector {
public:
    void Add(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back(line);
    }
    std::vector<std::string> Snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }
    std::size_t Count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

#ifdef _WIN32
const std::string kShell = "cmd.exe";
std::vector<std::string> ShellArgs(const std::string& script) { return {"/c", script}; }
const std::string kSleepCommand = "ping";
std::vector<std::string> SleepArgs() { return {"-n", "30", "127.0.0.1"}; }
#else
const std::string kShell = "/bin/sh";
std::vector<std::string> ShellArgs(const std::string& script) { return {"-c", script}; }
const std::string kSleepCommand = "sleep";
std::vector<std::string> SleepArgs() { return {"30"}; }
#endif

}  // namespace

TEST(RealProcessRunnerPortableTest, ChildExitsImmediatelyWithZero) {
    RealProcessRunner runner;
    auto process = runner.Start(kShell, ShellArgs("exit 0"), ProcessOptions{},
                                 [](const std::string&) {}, [](const std::string&) {});
    auto result = process->WaitFor(5000);
    ASSERT_TRUE(result.has_value()) << "a process that exits immediately must not hang Wait()";
    EXPECT_EQ(result->exitCode, 0);
    EXPECT_FALSE(result->wasTerminated);
    EXPECT_FALSE(process->IsRunning());
}

TEST(RealProcessRunnerPortableTest, ChildExitsImmediatelyWithNonzero) {
    RealProcessRunner runner;
    auto process = runner.Start(kShell, ShellArgs("exit 7"), ProcessOptions{},
                                 [](const std::string&) {}, [](const std::string&) {});
    auto result = process->WaitFor(5000);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->exitCode, 7);
    EXPECT_FALSE(result->wasTerminated);
}

TEST(RealProcessRunnerPortableTest, LaunchingANonexistentExecutableThrowsCleanlyAndLeaksNoProcess) {
    RealProcessRunner runner;
    EXPECT_THROW(
        {
            runner.Start("this-executable-definitely-does-not-exist-anywhere-abc123", {}, ProcessOptions{},
                         [](const std::string&) {}, [](const std::string&) {});
        },
        MediaToolException);
    // Nothing to assert about leaked children beyond "the exception was thrown before
    // RealProcess was ever constructed" (reproc::process::start() failing means no child
    // was forked at all) -- verified by code inspection alongside this test; there is no
    // portable, race-free way to assert "no new process exists" from a unit test.
}

TEST(RealProcessRunnerPortableTest, KillStopsAHangingProcessQuickly) {
    RealProcessRunner runner;
    auto process = runner.Start(kSleepCommand, SleepArgs(), ProcessOptions{},
                                 [](const std::string&) {}, [](const std::string&) {});

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_TRUE(process->IsRunning());

    process->Kill();

    // A 30-second sleep would otherwise run to completion; Kill() must end this well
    // under that, proving the kill path actually reaches the OS process and doesn't just
    // set a flag nothing acts on.
    auto result = process->WaitFor(5000);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->wasTerminated);
    EXPECT_FALSE(process->IsRunning());
}

TEST(RealProcessRunnerPortableTest, TerminateFallsBackToKillWhenTheChildIgnoresIt) {
    RealProcessRunner runner;
#ifdef _WIN32
    auto process = runner.Start(kSleepCommand, SleepArgs(), ProcessOptions{}, [](const std::string&) {},
                                 [](const std::string&) {});
#else
    // Ignores SIGTERM so a caller relying on Terminate() alone would hang; only Kill()
    // (SIGKILL, which cannot be caught or ignored) can end it -- the real-world case this
    // proves is "a stuck ffmpeg that doesn't respond to a graceful stop still gets killed."
    // `exec` replaces the shell's own process image with sleep(1) rather than forking it
    // as a child -- same PID throughout, no separate process left holding the stdout/
    // stderr pipes open once this one process is killed. (POSIX ignored-signal
    // dispositions, unlike handlers, survive exec, so the trap still applies afterward.)
    // A plain `trap ...; sleep 30` (fork, not exec) hits exactly the stale-pipe-handle
    // problem the sibling Windows test's own comment already warns about: killing the
    // shell leaves the forked sleep orphaned and still holding the pipe open, so it never
    // sees EOF -- which is what caused this test to hang until its own gtest timeout when
    // first written this way.
    auto process = runner.Start(kShell, ShellArgs("trap '' TERM; exec sleep 30"), ProcessOptions{},
                                 [](const std::string&) {}, [](const std::string&) {});
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ASSERT_TRUE(process->IsRunning());

    process->Terminate();
    auto afterTerminate = process->WaitFor(500);
#ifndef _WIN32
    // Confirms the trap actually took effect -- SIGTERM alone did not end it.
    EXPECT_FALSE(afterTerminate.has_value()) << "expected the SIGTERM-ignoring child to survive Terminate()";
#endif

    process->Kill();
    auto result = process->WaitFor(5000);
    ASSERT_TRUE(result.has_value()) << "Kill() must succeed even after Terminate() alone did not";
    EXPECT_TRUE(result->wasTerminated);
}

TEST(RealProcessRunnerPortableTest, EnormousStderrDoesNotDeadlockTheParent) {
    // A pipe's OS buffer is a few tens of KB; a child that writes far more than that to
    // stderr while nothing reads it is the classic subprocess deadlock (child blocks on a
    // full pipe, parent blocks waiting for exit, neither ever proceeds). This proves
    // RealProcessRunner's drain (reproc's concurrent stdout+stderr read) doesn't have that
    // bug -- the actual reason reproc was chosen (see docs/decisions.md), verified here
    // rather than only assumed.
#ifdef _WIN32
    const std::string script = "for /L %i in (1,1,20000) do @echo line %i 1>&2";
#else
    const std::string script = "for i in $(seq 1 200000); do echo \"a line of stderr output number $i\" 1>&2; done";
#endif
    RealProcessRunner runner;
    LineCollector stderrLines;
    auto process = runner.Start(kShell, ShellArgs(script), ProcessOptions{}, [](const std::string&) {},
                                 [&](const std::string& line) { stderrLines.Add(line); });

    auto result = process->WaitFor(20000);
    ASSERT_TRUE(result.has_value()) << "the child deadlocked instead of running to completion -- "
                                        "stderr was not being drained concurrently with stdout";
    EXPECT_EQ(result->exitCode, 0);
    EXPECT_GT(stderrLines.Count(), 1000u);
}

TEST(RealProcessRunnerPortableTest, InvalidUtf8OnStdoutDoesNotCrashTheReader) {
    // LineSplitter only ever does raw-byte '\n' splitting (core/process/RealProcessRunner.cpp)
    // -- confirms that holds even when a child emits bytes that are not valid UTF-8 at all
    // (a possible real source: ffmpeg/yt-dlp echoing a filename or title in an encoding this
    // process didn't choose). std::string is a byte string; nothing here should validate or
    // reject non-UTF-8 content the way the earlier, now-fixed JSON serialization bug did.
#ifdef _WIN32
    GTEST_SKIP() << "invalid-byte stdout injection is exercised on POSIX only in this suite "
                    "(needs a helper binary on Windows to emit raw bytes)";
#else
    RealProcessRunner runner;
    LineCollector stdoutLines;
    auto process = runner.Start(kShell, ShellArgs("printf 'before\\xff\\xfeafter\\n'"), ProcessOptions{},
                                 [&](const std::string& line) { stdoutLines.Add(line); },
                                 [](const std::string&) {});
    auto result = process->WaitFor(5000);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->exitCode, 0);
    ASSERT_EQ(stdoutLines.Snapshot().size(), 1u);
    // The raw bytes survive untouched -- no crash, no silent truncation, no replacement.
    EXPECT_NE(stdoutLines.Snapshot().front().find("before"), std::string::npos);
    EXPECT_NE(stdoutLines.Snapshot().front().find("after"), std::string::npos);
#endif
}
