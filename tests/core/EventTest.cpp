#include "core/events/Event.h"

#include <gtest/gtest.h>

namespace mediatool::events {
namespace {

struct WireCase {
    EventType type;
    const char* wire;
};

// One row per EventType declared in core/events/Event.h -- add a row here whenever a
// new EventType is added.
constexpr WireCase kAllEventTypes[] = {
    {EventType::JobCreated, "jobCreated"},
    {EventType::JobQueued, "jobQueued"},
    {EventType::JobStarted, "jobStarted"},
    {EventType::JobProgress, "jobProgress"},
    {EventType::JobPaused, "jobPaused"},
    {EventType::JobResumed, "jobResumed"},
    {EventType::JobCompleted, "jobCompleted"},
    {EventType::JobFailed, "jobFailed"},
    {EventType::JobCancelled, "jobCancelled"},
    {EventType::FileDetected, "fileDetected"},
    {EventType::HardwareDetected, "hardwareDetected"},
    {EventType::DownloadMetadataReceived, "downloadMetadataReceived"},
    {EventType::LogEvent, "logEvent"},
};

TEST(EventTest, ToWireStringMatchesContractForEveryEventType) {
    for (const auto& c : kAllEventTypes) {
        EXPECT_EQ(ToWireString(c.type), c.wire);
    }
}

TEST(EventTest, ToJsonOmitsJobIdKeyWhenAbsent) {
    const Event event = MakeEvent(EventType::HardwareDetected, nlohmann::json{{"hardwareInfo", nlohmann::json::object()}});

    const nlohmann::json json = event.ToJson();

    EXPECT_FALSE(json.contains("jobId"));
    EXPECT_EQ(json.at("event"), "hardwareDetected");
    EXPECT_TRUE(json.contains("timestamp"));
    EXPECT_FALSE(json.at("timestamp").get<std::string>().empty());
    EXPECT_TRUE(json.at("data").contains("hardwareInfo"));
    EXPECT_EQ(json.size(), 3u);
}

TEST(EventTest, ToJsonIncludesJobIdWhenPresent) {
    const Event event = MakeEvent(EventType::JobProgress, nlohmann::json{{"percentage", 42.5}}, "job-abc123");

    const nlohmann::json json = event.ToJson();

    ASSERT_TRUE(json.contains("jobId"));
    EXPECT_EQ(json.at("jobId"), "job-abc123");
    EXPECT_EQ(json.at("event"), "jobProgress");
    EXPECT_EQ(json.at("data").at("percentage"), 42.5);
    EXPECT_EQ(json.size(), 4u);
}

TEST(EventTest, MakeEventFillsTimestamp) {
    const Event event = MakeEvent(EventType::JobCreated, nlohmann::json::object(), "job-xyz");

    EXPECT_FALSE(event.timestampUtc.empty());
    ASSERT_TRUE(event.jobId.has_value());
    EXPECT_EQ(*event.jobId, "job-xyz");
}

}  // namespace
}  // namespace mediatool::events
