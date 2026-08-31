#include "core/ipc/LineReader.h"

#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace mediatool::ipc {
namespace {

TEST(LineReader, ReadsLinesAndReportsEndOfStream) {
    std::istringstream input("first\nsecond\n");

    ReadLineResult result = ReadBoundedLine(input, 1024);
    EXPECT_EQ(result.status, ReadLineStatus::Ok);
    EXPECT_EQ(result.line, "first");

    result = ReadBoundedLine(input, 1024);
    EXPECT_EQ(result.status, ReadLineStatus::Ok);
    EXPECT_EQ(result.line, "second");

    result = ReadBoundedLine(input, 1024);
    EXPECT_EQ(result.status, ReadLineStatus::EndOfStream);
    EXPECT_TRUE(result.line.empty());
}

TEST(LineReader, AFinalLineWithoutANewlineIsStillALine) {
    std::istringstream input("no trailing newline");
    const ReadLineResult result = ReadBoundedLine(input, 1024);
    EXPECT_EQ(result.status, ReadLineStatus::Ok);
    EXPECT_EQ(result.line, "no trailing newline");
    EXPECT_EQ(ReadBoundedLine(input, 1024).status, ReadLineStatus::EndOfStream);
}

TEST(LineReader, EmptyLinesAreReturnedAsEmptyNotAsEndOfStream) {
    std::istringstream input("\n\nx\n");
    ReadLineResult result = ReadBoundedLine(input, 1024);
    EXPECT_EQ(result.status, ReadLineStatus::Ok);
    EXPECT_TRUE(result.line.empty());
    result = ReadBoundedLine(input, 1024);
    EXPECT_EQ(result.status, ReadLineStatus::Ok);
    EXPECT_TRUE(result.line.empty());
    result = ReadBoundedLine(input, 1024);
    EXPECT_EQ(result.line, "x");
}

TEST(LineReader, ALineAtExactlyTheLimitIsAccepted) {
    std::istringstream input(std::string(64, 'a') + "\n");
    const ReadLineResult result = ReadBoundedLine(input, 64);
    EXPECT_EQ(result.status, ReadLineStatus::Ok);
    EXPECT_EQ(result.line.size(), 64u);
}

TEST(LineReader, AnOversizedLineIsRejectedWithoutBeingBuffered) {
    std::istringstream input(std::string(200, 'a') + "\n");
    const ReadLineResult result = ReadBoundedLine(input, 64);
    EXPECT_EQ(result.status, ReadLineStatus::LineTooLong);
    EXPECT_TRUE(result.line.empty()) << "the oversized content must not be handed back";
    EXPECT_EQ(result.bytesDiscarded, 201u) << "200 bytes plus the newline";
}

TEST(LineReader, TheLoopResumesCleanlyOnTheLineAfterAnOversizedOne) {
    // The behavior that keeps the IPC loop alive: one hostile line costs one request, not
    // the process and not the stream alignment.
    std::istringstream input("ok-before\n" + std::string(5000, 'x') + "\nok-after\n");

    EXPECT_EQ(ReadBoundedLine(input, 100).line, "ok-before");
    EXPECT_EQ(ReadBoundedLine(input, 100).status, ReadLineStatus::LineTooLong);

    const ReadLineResult after = ReadBoundedLine(input, 100);
    EXPECT_EQ(after.status, ReadLineStatus::Ok);
    EXPECT_EQ(after.line, "ok-after");
}

TEST(LineReader, AnUnterminatedOversizedLineEndsTheStreamWithoutSpinning) {
    // The original failure mode this bounds: a peer that opens a line and never closes it. The
    // read must terminate, and the caller's loop must then see end-of-stream rather than
    // being handed the same overlong line forever.
    std::istringstream input(std::string(10000, 'y'));
    const ReadLineResult result = ReadBoundedLine(input, 128);
    EXPECT_EQ(result.status, ReadLineStatus::LineTooLong);
    EXPECT_EQ(result.bytesDiscarded, 10000u);
    EXPECT_EQ(ReadBoundedLine(input, 128).status, ReadLineStatus::EndOfStream);
}

TEST(LineReader, AMegabyteScaleLineIsBoundedByTheLimitNotByTheInput) {
    // A stand-in for the audit's 100MB-line case, sized to keep the test fast: what
    // matters is that peak retained memory tracks the limit, not the line.
    const std::size_t kLimit = 1024;
    std::istringstream input(std::string(4 * 1024 * 1024, 'z') + "\n{\"id\":\"req-1\"}\n");

    const ReadLineResult oversized = ReadBoundedLine(input, kLimit);
    EXPECT_EQ(oversized.status, ReadLineStatus::LineTooLong);
    EXPECT_TRUE(oversized.line.empty()) << "nothing of the oversized line is retained";
    EXPECT_LE(oversized.line.capacity(), kLimit)
        << "peak retained capacity must track the limit, not the input";

    EXPECT_EQ(ReadBoundedLine(input, kLimit).line, "{\"id\":\"req-1\"}");
}

}  // namespace
}  // namespace mediatool::ipc
