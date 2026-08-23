#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "core/process/MockProcessRunner.h"

using mediatool::process::MockProcessRunner;
using mediatool::process::ProcessOptions;
using mediatool::process::ProcessResult;

TEST(MockProcessRunnerTest, DeliversExactlyItsCannedLinesAndExitCode) {
    MockProcessRunner runner({"line1", "line2"}, {"err1"}, 7);

    std::vector<std::string> stdoutLines;
    std::vector<std::string> stderrLines;

    auto process = runner.Start(
        "fake", {}, ProcessOptions{}, [&](const std::string& line) { stdoutLines.push_back(line); },
        [&](const std::string& line) { stderrLines.push_back(line); });

    ProcessResult result = process->Wait();

    EXPECT_EQ(stdoutLines, (std::vector<std::string>{"line1", "line2"}));
    EXPECT_EQ(stderrLines, (std::vector<std::string>{"err1"}));
    EXPECT_EQ(result.exitCode, 7);
    EXPECT_FALSE(result.wasTerminated);
}

TEST(MockProcessRunnerTest, RecordsWrittenLines) {
    MockProcessRunner runner({}, {}, 0);
    auto process = runner.Start("fake", {}, ProcessOptions{}, nullptr, nullptr);

    process->WriteLine("hello");
    process->WriteLine("world");

    EXPECT_EQ(runner.WrittenLines(), (std::vector<std::string>{"hello", "world"}));
}

TEST(MockProcessRunnerTest, TerminateMarksResultAsTerminated) {
    MockProcessRunner runner({}, {}, 0);
    auto process = runner.Start("fake", {}, ProcessOptions{}, nullptr, nullptr);

    process->Terminate();

    ProcessResult result = process->Wait();
    EXPECT_TRUE(result.wasTerminated);
}
