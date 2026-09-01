// End-to-end protocol tests against a real mediatool-core subprocess. See
// CoreProcessFixture.h for why this exists alongside the unit suites.
//
// What is NOT covered here, deliberately and for a reason worth writing down: anything
// that needs a path to survive paths::IsSafeUserSuppliedPath. That gate requires a
// Windows-shaped absolute path ("C:\..."), on purpose (this is a Windows-only app and the
// check must not vary with the build host) -- so on a POSIX build host no real temporary
// file can be handed to inspectFile, getCapabilities, or a CONVERSION job's inputPath. On
// Windows those paths do work; here they can only be exercised in their REJECTING
// direction, which several tests below do.

#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "tests/integration/CoreProcessFixture.h"

namespace mediatool::integration {
namespace {

class IpcProtocolTest : public ::testing::Test {
protected:
    void SetUp() override { core_ = std::make_unique<CoreProcess>(MEDIATOOL_CORE_EXECUTABLE); }
    void TearDown() override { core_.reset(); }

    std::unique_ptr<CoreProcess> core_;
};

TEST_F(IpcProtocolTest, EveryRequestGetsExactlyOneResponseCarryingItsOwnId) {
    // The bridge (core_bridge.rs) matches responses to pending requests purely by id. An
    // answer without one, or with the wrong one, leaves the caller waiting out the full
    // 30s timeout instead of failing.
    const auto first = core_->Send("listJobs", nlohmann::json::object());
    ASSERT_TRUE(first.has_value()) << "no response to listJobs";
    EXPECT_EQ(first->at("id").get<std::string>(), "req-1");
    EXPECT_TRUE(first->at("ok").get<bool>());
    EXPECT_TRUE(first->at("result").contains("jobs"));

    const auto second = core_->Send("getSettings", nlohmann::json::object());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->at("id").get<std::string>(), "req-2");
    EXPECT_TRUE(second->at("result").at("settings").contains("processing"));
}

TEST_F(IpcProtocolTest, AnUnknownCommandFailsWithAResponseRatherThanSilence) {
    const auto response = core_->Send("thisCommandDoesNotExist", nlohmann::json::object());
    ASSERT_TRUE(response.has_value());
    EXPECT_FALSE(response->at("ok").get<bool>());
    EXPECT_EQ(response->at("error").at("category").get<std::string>(), "UNKNOWN");
}

TEST_F(IpcProtocolTest, GarbageOnStdinDoesNotKillTheReadLoop) {
    // The whole point of the bounded/validated reader: one malformed line must cost that
    // line and nothing else. A loop that dies here takes every queued job with it.
    core_->SendRawLine("this is not json at all");
    core_->SendRawLine("{\"id\": 42, \"command\": \"listJobs\"}");   // non-string id
    core_->SendRawLine("{\"command\": \"listJobs\"}");                // no id at all
    core_->SendRawLine("");
    core_->SendRawLine("{\"id\":\"x\",\"command\":\"listJobs\",\"params\":\"not an object\"}");

    const auto response = core_->Send("listJobs", nlohmann::json::object());
    ASSERT_TRUE(response.has_value()) << "the read loop stopped answering after malformed input";
    EXPECT_TRUE(response->at("ok").get<bool>());
}

TEST_F(IpcProtocolTest, AnOversizedLineIsDiscardedAndTheLoopSurvives) {
    // No id is recoverable from a line that was never accumulated, so there is nobody to
    // answer -- but the process must still be there afterwards.
    const std::string huge = "{\"id\":\"big\",\"command\":\"listJobs\",\"pad\":\"" +
                              std::string(4 * 1024 * 1024, 'a') + "\"}";
    core_->SendRawLine(huge);

    const auto response = core_->Send("listJobs", nlohmann::json::object(),
                                       std::chrono::seconds(20));
    ASSERT_TRUE(response.has_value()) << "the read loop did not survive an oversized line";
    EXPECT_TRUE(response->at("ok").get<bool>());
}

TEST_F(IpcProtocolTest, AJobRunsToCompletionAndEmitsItsLifecycleInOrder) {
    const auto created = core_->Send("createJob", {{"type", "TEST"}});
    ASSERT_TRUE(created.has_value());
    ASSERT_TRUE(created->at("ok").get<bool>()) << created->dump();
    const std::string jobId = created->at("result").at("jobId").get<std::string>();

    ASSERT_TRUE(core_->WaitForEvent("jobCreated", jobId).has_value());
    ASSERT_TRUE(core_->WaitForEvent("jobStarted", jobId).has_value());
    ASSERT_TRUE(core_->WaitForEvent("jobProgress", jobId).has_value());
    const auto completed = core_->WaitForEvent("jobCompleted", jobId, std::chrono::seconds(20));
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->json.at("data").at("state").get<std::string>(), "COMPLETED");

    // Ordering, not just presence: a consumer that renders these in arrival order must
    // never see the completion before the start.
    const auto events = core_->EventsSoFar();
    int createdIndex = -1;
    int completedIndex = -1;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].json.value("jobId", std::string()) != jobId) continue;
        if (events[i].EventName() == "jobCreated" && createdIndex < 0) createdIndex = static_cast<int>(i);
        if (events[i].EventName() == "jobCompleted") completedIndex = static_cast<int>(i);
    }
    ASSERT_GE(createdIndex, 0);
    ASSERT_GE(completedIndex, 0);
    EXPECT_LT(createdIndex, completedIndex);

    const auto snapshot = core_->Send("getJob", {{"jobId", jobId}});
    ASSERT_TRUE(snapshot.has_value());
    const auto& job = snapshot->at("result").at("job");
    EXPECT_EQ(job.at("state").get<std::string>(), "COMPLETED");
    // Phase C surfaced this so a UI can say "attempt 1 of 3" rather than nothing.
    EXPECT_EQ(job.at("attempts").get<int>(), 1);
}

TEST_F(IpcProtocolTest, CancellingARunningJobReachesCancelledNotCompleted) {
    const auto created = core_->Send("createJob", {{"type", "TEST"}});
    ASSERT_TRUE(created.has_value());
    const std::string jobId = created->at("result").at("jobId").get<std::string>();
    ASSERT_TRUE(core_->WaitForEvent("jobStarted", jobId).has_value());

    const auto cancelled = core_->Send("cancelJob", {{"jobId", jobId}});
    ASSERT_TRUE(cancelled.has_value());
    EXPECT_TRUE(cancelled->at("ok").get<bool>()) << cancelled->dump();

    ASSERT_TRUE(core_->WaitForEvent("jobCancelled", jobId, std::chrono::seconds(20)).has_value());
    const auto snapshot = core_->Send("getJob", {{"jobId", jobId}});
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->at("result").at("job").at("state").get<std::string>(), "CANCELLED");
}

TEST_F(IpcProtocolTest, CancellingTwiceIsNotAnError) {
    // Cancelling something that just finished is a normal outcome of a UI race, not a
    // failure the user should see reported.
    const auto created = core_->Send("createJob", {{"type", "TEST"}});
    ASSERT_TRUE(created.has_value());
    const std::string jobId = created->at("result").at("jobId").get<std::string>();

    ASSERT_TRUE(core_->Send("cancelJob", {{"jobId", jobId}})->at("ok").get<bool>());
    const auto again = core_->Send("cancelJob", {{"jobId", jobId}});
    ASSERT_TRUE(again.has_value());
    EXPECT_TRUE(again->at("ok").get<bool>()) << again->dump();
}

TEST_F(IpcProtocolTest, AnUnknownJobIdIsReportedNotIgnored) {
    const auto response = core_->Send("getJob", {{"jobId", "job-does-not-exist"}});
    ASSERT_TRUE(response.has_value());
    EXPECT_FALSE(response->at("ok").get<bool>());
    EXPECT_EQ(response->at("error").at("code").get<std::string>(), "E_JOB_NOT_FOUND");
}

TEST_F(IpcProtocolTest, ParameterValidationFailuresNameTheOffendingField) {
    const auto missing = core_->Send("createJob", nlohmann::json::object());
    ASSERT_TRUE(missing.has_value());
    EXPECT_FALSE(missing->at("ok").get<bool>());
    EXPECT_FALSE(missing->at("error").at("details").get<std::string>().empty());

    const auto outOfRange =
        core_->Send("createJob", {{"type", "TEST"}, {"params", {{"priority", 999999}}}});
    ASSERT_TRUE(outOfRange.has_value());
    EXPECT_FALSE(outOfRange->at("ok").get<bool>());
    EXPECT_EQ(outOfRange->at("error").at("code").get<std::string>(), "E_INVALID_PARAM_VALUE");
}

TEST_F(IpcProtocolTest, ATraversalPathIsRejectedAtTheBoundary) {
    const auto response = core_->Send(
        "createJob", {{"type", "CONVERSION"},
                       {"params",
                        {{"inputPath", "C:\\Users\\hamim\\..\\..\\Windows\\System32\\config\\SAM"},
                         {"outputDirectory", "C:\\out"},
                         {"options", {{"outputFormat", "mp4"}}}}}});
    ASSERT_TRUE(response.has_value());
    EXPECT_FALSE(response->at("ok").get<bool>());
    EXPECT_EQ(response->at("error").at("code").get<std::string>(), "E_INVALID_INPUT_PATH");
}

TEST_F(IpcProtocolTest, AHostileFormatIdIsRejectedBeforeAnyJobIsCreated) {
    // The full Phase A path, exercised through the real boundary: -f is an expression
    // language, and "all" means "download every stream on the page".
    const auto response =
        core_->Send("createJob", {{"type", "DOWNLOAD"},
                                   {"params",
                                    {{"url", "https://example.com/watch?v=abc"},
                                     {"outputDirectory", "C:\\out"},
                                     {"formatId", "all"}}}});
    ASSERT_TRUE(response.has_value());
    EXPECT_FALSE(response->at("ok").get<bool>());
    EXPECT_EQ(response->at("error").at("code").get<std::string>(), "E_INVALID_FORMAT_ID");

    // No job was created, so nothing is left behind to cancel or clean up.
    const auto jobs = core_->Send("listJobs", nlohmann::json::object());
    ASSERT_TRUE(jobs.has_value());
    EXPECT_TRUE(jobs->at("result").at("jobs").empty());
}

TEST_F(IpcProtocolTest, SettingsRoundTripThroughTheRealFileAndValidationRejectsBadValues) {
    const auto updated =
        core_->Send("updateSettings", {{"settings", {{"processing", {{"maxRetryAttempts", 5}}}}}});
    ASSERT_TRUE(updated.has_value());
    ASSERT_TRUE(updated->at("ok").get<bool>()) << updated->dump();
    EXPECT_EQ(updated->at("result").at("settings").at("processing").at("maxRetryAttempts").get<int>(), 5);

    const auto readBack = core_->Send("getSettings", nlohmann::json::object());
    ASSERT_TRUE(readBack.has_value());
    EXPECT_EQ(readBack->at("result").at("settings").at("processing").at("maxRetryAttempts").get<int>(),
               5);

    const auto rejected =
        core_->Send("updateSettings", {{"settings", {{"processing", {{"maxRetryAttempts", 99}}}}}});
    ASSERT_TRUE(rejected.has_value());
    EXPECT_FALSE(rejected->at("ok").get<bool>());

    // Refused, not clamped: the stored value is still the one that was accepted.
    const auto unchanged = core_->Send("getSettings", nlohmann::json::object());
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_EQ(unchanged->at("result").at("settings").at("processing").at("maxRetryAttempts").get<int>(),
               5);
}

TEST_F(IpcProtocolTest, DependenciesOrderExecutionAndAnUnknownOneIsRefusedAtSubmission) {
    const auto first = core_->Send("createJob", {{"type", "TEST"}});
    ASSERT_TRUE(first.has_value());
    const std::string firstId = first->at("result").at("jobId").get<std::string>();

    const auto second = core_->Send(
        "createJob", {{"type", "TEST"}, {"params", {{"dependsOn", nlohmann::json::array({firstId})}}}});
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(second->at("ok").get<bool>()) << second->dump();
    const std::string secondId = second->at("result").at("jobId").get<std::string>();

    ASSERT_TRUE(core_->WaitForEvent("jobCompleted", firstId, std::chrono::seconds(30)).has_value());
    ASSERT_TRUE(core_->WaitForEvent("jobCompleted", secondId, std::chrono::seconds(30)).has_value());

    const auto bad = core_->Send(
        "createJob",
        {{"type", "TEST"}, {"params", {{"dependsOn", nlohmann::json::array({"job-nonexistent"})}}}});
    ASSERT_TRUE(bad.has_value());
    EXPECT_FALSE(bad->at("ok").get<bool>());
    EXPECT_EQ(bad->at("error").at("code").get<std::string>(), "E_INVALID_DEPENDENCY");
}

TEST_F(IpcProtocolTest, ClosingStdinShutsTheCoreDownCleanly) {
    // Not a formality: the teardown path is where the worker pool is joined and the
    // persistence stores are written, and it is the path that used to touch a destroyed
    // JobHistoryStore when jobs were still queued.
    ASSERT_TRUE(core_->Send("createJob", {{"type", "TEST"}}).has_value());
    ASSERT_TRUE(core_->Send("createJob", {{"type", "TEST"}}).has_value());
    ASSERT_TRUE(core_->Send("createJob", {{"type", "TEST"}}).has_value());

    EXPECT_EQ(core_->Shutdown(), 0);
}

}  // namespace
}  // namespace mediatool::integration
