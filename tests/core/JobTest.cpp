#include <gtest/gtest.h>

#include <set>
#include <string>
#include <string_view>

#include "core/common/IClock.h"
#include "core/errors/ErrorInfo.h"
#include "core/jobs/JobTypes.h"
#include "core/jobs/TestJob.h"

using mediatool::errors::ErrorCategory;
using mediatool::errors::ErrorInfo;
using mediatool::jobs::GenerateJobId;
using mediatool::jobs::JobState;
using mediatool::jobs::Progress;
using mediatool::jobs::TestJob;

namespace {

// Deterministic clock for timestamp assertions -- each call advances so
// created/started/completed are each distinguishable.
class FakeClock final : public mediatool::common::IClock {
public:
    std::string NowIso8601Utc() const override {
        return "2026-08-23T00:00:0" + std::to_string(callCount_++) + ".000Z";
    }

private:
    mutable int callCount_ = 0;
};

}  // namespace

TEST(JobId, GeneratedIdsHaveExpectedFormat) {
    const auto id = GenerateJobId();
    EXPECT_EQ(id.rfind("job-", 0), 0u) << "id=" << id;
    EXPECT_EQ(id.size(), std::string_view("job-").size() + 36u);
}

TEST(JobId, ManyGeneratedIdsAreUnique) {
    std::set<std::string> seen;
    constexpr int kCount = 20000;
    for (int i = 0; i < kCount; ++i) {
        seen.insert(GenerateJobId());
    }
    EXPECT_EQ(seen.size(), static_cast<size_t>(kCount));
}

TEST(Job, TimestampsAreSetByLifecycleMethods) {
    FakeClock clock;
    TestJob job(clock);

    // createdAt is stamped at construction time.
    EXPECT_FALSE(job.CreatedAt().empty());
    EXPECT_FALSE(job.StartedAt().has_value());
    EXPECT_FALSE(job.CompletedAt().has_value());

    job.MarkStarting();
    EXPECT_EQ(job.State(), JobState::Starting);
    EXPECT_FALSE(job.StartedAt().has_value());  // startedAt is set on MarkRunning

    job.MarkRunning();
    EXPECT_EQ(job.State(), JobState::Running);
    ASSERT_TRUE(job.StartedAt().has_value());

    job.MarkCompleted();
    EXPECT_EQ(job.State(), JobState::Completed);
    ASSERT_TRUE(job.CompletedAt().has_value());
}

// Regression test for #17: MarkRetrying() previously cleared cancellationRequested_ but
// left the prior run's error_/progress_ in place, so a retried job's snapshot reported
// the stale failure and stale byte counts until the new run's own progress events
// happened to overwrite them.
TEST(Job, MarkRetryingClearsStaleErrorAndProgress) {
    TestJob job;
    job.MarkStarting();
    job.MarkRunning();

    Progress midProgress;
    midProgress.statusMessage = "Halfway done";
    midProgress.percentage = 50.0;
    job.ReportProgress(midProgress);
    EXPECT_EQ(job.GetProgress().percentage, 50.0);

    job.MarkFailed(ErrorInfo::Make("E_TEST_FAILURE", ErrorCategory::Unknown, "boom"));
    ASSERT_TRUE(job.GetError().has_value());

    job.MarkRetrying();

    EXPECT_FALSE(job.GetError().has_value());
    EXPECT_FALSE(job.GetProgress().percentage.has_value());
    EXPECT_EQ(job.GetProgress().statusMessage, "Retrying");
}

TEST(Job, TestJobRunsDirectlyAndReportsIncreasingProgress) {
    TestJob job;

    std::vector<double> percentages;
    job.SetCallbacks(
        /*onStateChanged=*/nullptr,
        [&percentages](const Progress& progress) {
            if (progress.percentage) percentages.push_back(*progress.percentage);
        });

    job.MarkStarting();
    job.MarkRunning();
    job.Execute();
    job.MarkCompleted();

    EXPECT_EQ(job.State(), JobState::Completed);
    ASSERT_TRUE(job.GetResult().has_value());
    EXPECT_TRUE((*job.GetResult()).contains("message"));

    ASSERT_GE(percentages.size(), 2u);
    for (size_t i = 1; i < percentages.size(); ++i) {
        EXPECT_GT(percentages[i], percentages[i - 1]);
    }
    EXPECT_DOUBLE_EQ(percentages.back(), 100.0);
}
