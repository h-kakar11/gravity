// Measured baselines for the things a user actually waits on, taken against a real
// mediatool-core subprocess over the real protocol.
//
// Two rules make these worth having rather than a source of noise.
//
// First, they PRINT every measurement, always, so a run is a record and not just a
// pass/fail. Regressions in this kind of number are gradual, and a test that only speaks
// up when it crosses a line hides the slide toward it.
//
// Second, the ceilings they assert are deliberately several times the measured value --
// they are there to catch something going structurally wrong (a lock held across a
// process spawn, an O(n^2) queue insert), not to enforce a target. A benchmark that fails
// because CI was busy gets re-run until it passes, which is the same as not having it.
//
// The numbers this produces on a reference machine are published in docs/performance.md.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "tests/integration/CoreProcessFixture.h"

namespace mediatool::integration {
namespace {

using Clock = std::chrono::steady_clock;

double MillisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void Report(const std::string& what, double measuredMs, double ceilingMs) {
    std::printf("[ BASELINE ] %-46s %8.1f ms  (ceiling %.0f ms)\n", what.c_str(), measuredMs,
                 ceilingMs);
    std::fflush(stdout);
}

// The median is the honest summary for latency under a scheduler: one 40ms outlier from a
// context switch says nothing about the operation.
double Median(std::vector<double> samples) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

class PerformanceBaselineTest : public ::testing::Test {
protected:
    void SetUp() override { core_ = std::make_unique<CoreProcess>(MEDIATOOL_CORE_EXECUTABLE); }
    void TearDown() override { core_.reset(); }

    std::unique_ptr<CoreProcess> core_;
};

TEST_F(PerformanceBaselineTest, ColdStartToFirstAnswer) {
    // Everything AppContext does before it can answer anything: settings load, ffmpeg
    // discovery, the encoder probe, the worker pool, the crash-recovery pass. This is the
    // number that decides whether the app feels like it launched or like it hung.
    const auto start = Clock::now();
    CoreProcess core(MEDIATOOL_CORE_EXECUTABLE);
    const auto response = core.Send("listJobs", nlohmann::json::object(), std::chrono::seconds(30));
    const double elapsed = MillisecondsSince(start);

    ASSERT_TRUE(response.has_value());
    Report("cold start -> first answered request", elapsed, 5000);
    EXPECT_LT(elapsed, 5000.0);
}

TEST_F(PerformanceBaselineTest, RequestResponseRoundTrip) {
    // The floor under every other number: one line in, one line out, no work in between.
    // A regression here is a regression in the IPC loop itself.
    (void)core_->Send("listJobs", nlohmann::json::object());  // warm up

    std::vector<double> samples;
    for (int i = 0; i < 40; ++i) {
        const auto start = Clock::now();
        const auto response = core_->Send("listJobs", nlohmann::json::object());
        ASSERT_TRUE(response.has_value());
        samples.push_back(MillisecondsSince(start));
    }

    const double median = Median(samples);
    Report("listJobs round trip (median of 40)", median, 100);
    EXPECT_LT(median, 100.0);
}

TEST_F(PerformanceBaselineTest, JobStartLatency) {
    // createJob returning is not the same as the job running. This is the gap the user
    // sees between pressing a button and the first progress bar moving.
    std::vector<double> samples;
    for (int i = 0; i < 10; ++i) {
        const auto start = Clock::now();
        const auto created = core_->Send("createJob", {{"type", "TEST"}});
        ASSERT_TRUE(created.has_value());
        const std::string jobId = created->at("result").at("jobId").get<std::string>();
        ASSERT_TRUE(core_->WaitForEvent("jobStarted", jobId, std::chrono::seconds(20)).has_value());
        samples.push_back(MillisecondsSince(start));
        ASSERT_TRUE(core_->Send("cancelJob", {{"jobId", jobId}}).has_value());
    }

    const double median = Median(samples);
    Report("createJob -> jobStarted event (median of 10)", median, 2000);
    EXPECT_LT(median, 2000.0);
}

TEST_F(PerformanceBaselineTest, FirstProgressEventLatency) {
    // "Progress events <1s" from the plan: how long a user stares at a started job before
    // it tells them anything.
    const auto created = core_->Send("createJob", {{"type", "TEST"}});
    ASSERT_TRUE(created.has_value());
    const std::string jobId = created->at("result").at("jobId").get<std::string>();

    const auto start = Clock::now();
    ASSERT_TRUE(core_->WaitForEvent("jobProgress", jobId, std::chrono::seconds(20)).has_value());
    const double elapsed = MillisecondsSince(start);

    Report("createJob -> first jobProgress event", elapsed, 3000);
    EXPECT_LT(elapsed, 3000.0);
    (void)core_->Send("cancelJob", {{"jobId", jobId}});
}

TEST_F(PerformanceBaselineTest, HundredJobQueueOperations) {
    // Submitting a hundred jobs, listing them, and cancelling them all. The scheduler
    // stores its pending set in scheduling order precisely so this stays linear -- an
    // O(n) insert that also does a lookup per element makes enqueuing quadratic while
    // JobManager's lock is held, which is what this would catch.
    constexpr int kJobs = 100;

    std::vector<std::string> ids;
    ids.reserve(kJobs);
    const auto submitStart = Clock::now();
    for (int i = 0; i < kJobs; ++i) {
        const auto created = core_->Send("createJob", {{"type", "TEST"}});
        ASSERT_TRUE(created.has_value()) << "failed at job " << i;
        ids.push_back(created->at("result").at("jobId").get<std::string>());
    }
    const double submitMs = MillisecondsSince(submitStart);
    Report("submit 100 jobs (total)", submitMs, 30000);
    Report("  ...per job", submitMs / kJobs, 300);
    EXPECT_LT(submitMs, 30000.0);

    const auto listStart = Clock::now();
    const auto listed = core_->Send("listJobs", nlohmann::json::object(), std::chrono::seconds(30));
    const double listMs = MillisecondsSince(listStart);
    ASSERT_TRUE(listed.has_value());
    EXPECT_EQ(listed->at("result").at("jobs").size(), static_cast<std::size_t>(kJobs));
    Report("listJobs with 100 jobs", listMs, 1000);
    EXPECT_LT(listMs, 1000.0);

    const auto cancelStart = Clock::now();
    for (const std::string& id : ids) {
        ASSERT_TRUE(core_->Send("cancelJob", {{"jobId", id}}).has_value());
    }
    const double cancelMs = MillisecondsSince(cancelStart);
    Report("cancel 100 jobs (total)", cancelMs, 30000);
    EXPECT_LT(cancelMs, 30000.0);
}

TEST_F(PerformanceBaselineTest, ShutdownWithAFullQueue) {
    // Shutdown cancels the queue rather than draining it, so this must not scale with how
    // much work was pending. A regression here reads to a user as "the app won't close".
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(core_->Send("createJob", {{"type", "TEST"}}).has_value());
    }

    const auto start = Clock::now();
    const int exitCode = core_->Shutdown();
    const double elapsed = MillisecondsSince(start);

    EXPECT_EQ(exitCode, 0);
    Report("shutdown with 100 jobs queued", elapsed, 10000);
    EXPECT_LT(elapsed, 10000.0);
}

}  // namespace
}  // namespace mediatool::integration
