#include <gtest/gtest.h>

#include "tests/support/PlatformTest.h"

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/process/RealProcessRunner.h"

using mediatool::process::ProcessOptions;
using mediatool::process::ProcessResult;
using mediatool::process::RealProcessRunner;

namespace {

// Callbacks fire on the IProcess's background drain thread; collect under a mutex so
// assertions on the test thread see a consistent snapshot.
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

private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

}  // namespace

TEST(RealProcessRunnerTest, EchoProducesExpectedStdoutLine) {
    SKIP_UNLESS_WINDOWS();
    RealProcessRunner runner;
    LineCollector stdoutLines;
    LineCollector stderrLines;

    auto process = runner.Start(
        "cmd.exe", {"/c", "echo hello"}, ProcessOptions{},
        [&](const std::string& line) { stdoutLines.Add(line); },
        [&](const std::string& line) { stderrLines.Add(line); });

    ProcessResult result = process->Wait();

    EXPECT_EQ(result.exitCode, 0);
    EXPECT_FALSE(result.wasTerminated);

    auto lines = stdoutLines.Snapshot();
    ASSERT_FALSE(lines.empty());
    EXPECT_EQ(lines.front(), "hello");
}

TEST(RealProcessRunnerTest, KillStopsLongRunningProcessQuickly) {
    SKIP_UNLESS_WINDOWS();
    RealProcessRunner runner;

    // Deliberately "ping.exe" directly rather than "cmd.exe /c ping ...": cmd.exe would
    // spawn ping.exe as its own child, which inherits a duplicate handle to our stdout
    // pipe -- killing cmd.exe alone then leaves that handle open and the pipe never sees
    // EOF until ping.exe finishes on its own. Every real caller in this codebase (ffmpeg,
    // python) is launched directly, never through a shell wrapper, so this matches actual
    // usage and correctly exercises "Kill() stops the process this runner started".
    auto process = runner.Start("ping", {"-n", "30", "127.0.0.1"}, ProcessOptions{},
                                 [](const std::string&) {}, [](const std::string&) {});

    // Let the child actually start before trying to kill it.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_TRUE(process->IsRunning());

    process->Kill();

    // A 30-round ping would otherwise take ~30 seconds; killing it must end the test
    // in well under that.
    auto result = process->WaitFor(5000);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->wasTerminated);
    EXPECT_FALSE(process->IsRunning());
}
