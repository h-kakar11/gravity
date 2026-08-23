#include "core/downloads/NdjsonLineProtocol.h"

#include <gtest/gtest.h>

using mediatool::downloads::DownloaderEventType;
using mediatool::downloads::GetDownloaderEventType;
using mediatool::downloads::IsCompletedEvent;
using mediatool::downloads::IsErrorEvent;
using mediatool::downloads::IsMetadataEvent;
using mediatool::downloads::IsProgressEvent;
using mediatool::downloads::ParseNdjsonLine;

TEST(NdjsonLineProtocol, ParsesValidJsonObject) {
    const auto parsed = ParseNdjsonLine(R"({"event":"completed","data":{"outputPath":"C:\\out.mp4"}})");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ((*parsed)["event"], "completed");
}

TEST(NdjsonLineProtocol, RejectsEmptyLine) {
    EXPECT_FALSE(ParseNdjsonLine("").has_value());
}

TEST(NdjsonLineProtocol, RejectsMalformedJson) {
    EXPECT_FALSE(ParseNdjsonLine("{not valid json").has_value());
    EXPECT_FALSE(ParseNdjsonLine("{\"event\": \"progress\", }").has_value());
}

TEST(NdjsonLineProtocol, RejectsNonObjectTopLevelValues) {
    EXPECT_FALSE(ParseNdjsonLine("[1,2,3]").has_value());
    EXPECT_FALSE(ParseNdjsonLine("\"just a string\"").has_value());
    EXPECT_FALSE(ParseNdjsonLine("42").has_value());
    EXPECT_FALSE(ParseNdjsonLine("null").has_value());
}

// The following four lines are the exact examples from docs/ipc-contract.md's
// "Python downloader (downloader.py) <-> C++ core" section.

TEST(NdjsonLineProtocol, RecognizesMetadataEvent) {
    const auto parsed = ParseNdjsonLine(
        R"({"event": "metadata", "data": {"title": "...", "duration": 123, "playlistIndex": null, "playlistCount": null}})");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(IsMetadataEvent(*parsed));
    EXPECT_FALSE(IsProgressEvent(*parsed));
    EXPECT_FALSE(IsCompletedEvent(*parsed));
    EXPECT_FALSE(IsErrorEvent(*parsed));
    EXPECT_EQ(GetDownloaderEventType(*parsed), DownloaderEventType::Metadata);
}

TEST(NdjsonLineProtocol, RecognizesProgressEvent) {
    const auto parsed = ParseNdjsonLine(
        R"({"event": "progress", "data": {"downloadedBytes": 1048576, "totalBytes": 52428800, "speedBytesPerSecond": 2097152, "etaSeconds": 24, "statusMessage": "Downloading"}})");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(IsProgressEvent(*parsed));
    EXPECT_FALSE(IsMetadataEvent(*parsed));
    EXPECT_EQ(GetDownloaderEventType(*parsed), DownloaderEventType::Progress);
}

TEST(NdjsonLineProtocol, RecognizesCompletedEvent) {
    const auto parsed = ParseNdjsonLine(R"({"event": "completed", "data": {"outputPath": "D:\\Videos\\My Video.mp4"}})");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(IsCompletedEvent(*parsed));
    EXPECT_EQ(GetDownloaderEventType(*parsed), DownloaderEventType::Completed);
    EXPECT_EQ((*parsed)["data"]["outputPath"], "D:\\Videos\\My Video.mp4");
}

TEST(NdjsonLineProtocol, RecognizesErrorEvent) {
    const auto parsed = ParseNdjsonLine(
        R"({"event": "error", "data": {"code": "E_NETWORK", "category": "NETWORK_ERROR", "message": "...", "details": "...", "recoverable": true}})");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(IsErrorEvent(*parsed));
    EXPECT_EQ(GetDownloaderEventType(*parsed), DownloaderEventType::Error);
}

TEST(NdjsonLineProtocol, UnrecognizedEventNameIsUnknown) {
    const auto parsed = ParseNdjsonLine(R"({"event": "somethingElse", "data": {}})");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(GetDownloaderEventType(*parsed), DownloaderEventType::Unknown);
    EXPECT_FALSE(IsMetadataEvent(*parsed));
    EXPECT_FALSE(IsProgressEvent(*parsed));
    EXPECT_FALSE(IsCompletedEvent(*parsed));
    EXPECT_FALSE(IsErrorEvent(*parsed));
}

TEST(NdjsonLineProtocol, MissingEventFieldIsUnknown) {
    const auto parsed = ParseNdjsonLine(R"({"data": {}})");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(GetDownloaderEventType(*parsed), DownloaderEventType::Unknown);
}
