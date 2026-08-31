#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/common/IClock.h"
#include "core/errors/ErrorInfo.h"
#include "core/jobs/Job.h"
#include "core/jobs/JobStateMachine.h"
#include "core/jobs/JobTypes.h"
#include "core/jobs/TestJob.h"

using mediatool::jobs::GenerateJobId;
using mediatool::jobs::Job;
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

// --- Transition semantics (TransitionResult) -------------------------------------------
// The whole point of the TransitionResult API is that losing a race is a *value*, not an
// exception crossing a thread boundary (see core/jobs/JobStateMachine.h). These tests pin
// each of the four outcomes to a concrete scenario, because "returns something non-Success"
// would not catch a regression that collapses AlreadyTerminal into InvalidTransition and
// starts logging benign races as state-machine bugs.

// A job that returns from Execute() the moment it is asked to, so lifecycle transitions can
// be driven by hand without the 1s TestJob sleep.
namespace {
class NoopJob final : public mediatool::jobs::Job {
public:
    NoopJob() : Job(mediatool::jobs::JobType::Test) {}
    void Execute() override {}
};
}  // namespace

using mediatool::jobs::TransitionResult;

TEST(JobTransitions, LegalTransitionsReportSuccess) {
    NoopJob job;
    EXPECT_EQ(job.MarkStarting(), TransitionResult::Success);
    EXPECT_EQ(job.MarkRunning(), TransitionResult::Success);
    EXPECT_EQ(job.MarkCompleted(), TransitionResult::Success);
}

TEST(JobTransitions, RepeatingATransitionIsAnIdempotentNoOp) {
    NoopJob job;
    ASSERT_EQ(job.MarkStarting(), TransitionResult::Success);
    ASSERT_EQ(job.MarkRunning(), TransitionResult::Success);
    ASSERT_EQ(job.MarkCancelled(), TransitionResult::Success);

    // Cancelling an already-cancelled job is the canonical idempotent case: the caller's
    // intent already holds, so this is AlreadyInState, not an error of any kind.
    EXPECT_EQ(job.MarkCancelled(), TransitionResult::AlreadyInState);
    EXPECT_EQ(job.State(), JobState::Cancelled);
}

TEST(JobTransitions, LosingARaceToATerminalStateReportsAlreadyTerminal) {
    NoopJob job;
    ASSERT_EQ(job.MarkStarting(), TransitionResult::Success);
    ASSERT_EQ(job.MarkRunning(), TransitionResult::Success);
    ASSERT_EQ(job.MarkCancelled(), TransitionResult::Success);

    // This is the exact shape of the #4 race: a worker thread finishing its work and
    // recording the outcome, after a user's cancellation already finalized the job. It
    // must be reported as "someone else got there first", never as a bug, and never by
    // throwing out of a worker thread.
    EXPECT_EQ(job.MarkCompleted(), TransitionResult::AlreadyTerminal);
    EXPECT_EQ(job.MarkFailed(mediatool::errors::ErrorInfo::Make(
                  "E_TEST", mediatool::errors::ErrorCategory::Unknown, "boom")),
              TransitionResult::AlreadyTerminal);

    // The terminal state that got there first stands, and the discarded error never
    // becomes visible on the job.
    EXPECT_EQ(job.State(), JobState::Cancelled);
    EXPECT_FALSE(job.GetError().has_value());
}

TEST(JobTransitions, IllegalTransitionFromANonTerminalStateReportsInvalidTransition) {
    NoopJob job;
    // QUEUED -> RUNNING skips STARTING: not a race, a caller that does not understand the
    // lifecycle. This is the one result worth logging as a defect.
    EXPECT_EQ(job.MarkRunning(), TransitionResult::InvalidTransition);
    EXPECT_EQ(job.State(), JobState::Queued);
    EXPECT_EQ(job.MarkCompleted(), TransitionResult::InvalidTransition);
    EXPECT_EQ(job.State(), JobState::Queued);
}

TEST(JobTransitions, NoTransitionMethodEverThrows) {
    // Exhaustive: every transition entry point, called from every reachable state,
    // including the illegal combinations. An exception escaping any of these on a worker
    // thread is what took the whole process down in #4, so the guarantee is "none of
    // these throw", not "the ones we happen to call don't throw".
    const auto exerciseEveryTransition = [](Job& job) {
        EXPECT_NO_THROW({
            job.MarkStarting();
            job.MarkRunning();
            job.MarkCompleted();
            job.MarkFailed(mediatool::errors::ErrorInfo::Make(
                "E_TEST", mediatool::errors::ErrorCategory::Unknown, "boom"));
            job.MarkCancelled();
            job.MarkRetrying();
            job.RequestCancel();
            job.RequestPause();
            job.RequestResume();
        });
    };

    NoopJob fresh;
    exerciseEveryTransition(fresh);

    NoopJob cancelled;
    cancelled.RequestCancel();
    exerciseEveryTransition(cancelled);

    NoopJob completed;
    completed.MarkStarting();
    completed.MarkRunning();
    completed.MarkCompleted();
    exerciseEveryTransition(completed);
}

TEST(JobTransitions, RetryingIsTheOnlyWayOutOfATerminalState) {
    NoopJob job;
    ASSERT_EQ(job.MarkStarting(), TransitionResult::Success);
    ASSERT_EQ(job.MarkRunning(), TransitionResult::Success);
    ASSERT_EQ(job.MarkFailed(mediatool::errors::ErrorInfo::Make(
                  "E_TEST", mediatool::errors::ErrorCategory::Unknown, "boom")),
              TransitionResult::Success);

    EXPECT_EQ(job.MarkRetrying(), TransitionResult::Success);
    EXPECT_EQ(job.State(), JobState::Retrying);
    EXPECT_FALSE(job.CompletedAt().has_value()) << "a retried job is no longer completed";
    EXPECT_EQ(job.MarkRetrying(), TransitionResult::AlreadyInState);

    NoopJob completed;
    completed.MarkStarting();
    completed.MarkRunning();
    completed.MarkCompleted();
    EXPECT_EQ(completed.MarkRetrying(), TransitionResult::InvalidTransition)
        << "COMPLETED is final -- only FAILED may be retried";
}

TEST(JobTransitions, CancelIsIdempotentAcrossRepeatedCalls) {
    NoopJob job;
    EXPECT_EQ(job.RequestCancel(), TransitionResult::Success);
    EXPECT_EQ(job.State(), JobState::Cancelled);
    // Second cancel: already terminal. Callers (JobManager::Shutdown cancels the queue,
    // then a user may cancel the same job) rely on this being a silent no-op.
    EXPECT_EQ(job.RequestCancel(), TransitionResult::AlreadyTerminal);
    EXPECT_EQ(job.State(), JobState::Cancelled);
}

TEST(JobTransitions, ConcurrentFinalizersProduceExactlyOneWinnerAndOneCallback) {
    // The multi-threaded version of the AlreadyTerminal test: 32 threads all trying to
    // finalize the same running job at once. Exactly one may win, the state-changed
    // callback must fire exactly once for the winning state, and nothing may throw.
    constexpr int kThreads = 32;
    for (int iteration = 0; iteration < 50; ++iteration) {
        NoopJob job;
        ASSERT_EQ(job.MarkStarting(), TransitionResult::Success);
        ASSERT_EQ(job.MarkRunning(), TransitionResult::Success);

        std::atomic<int> terminalCallbacks{0};
        job.SetCallbacks(
            [&terminalCallbacks](JobState state) {
                if (mediatool::jobs::IsTerminalState(state)) ++terminalCallbacks;
            },
            nullptr);

        std::atomic<int> successes{0};
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int i = 0; i < kThreads; ++i) {
            threads.emplace_back([&job, &successes, &go, i] {
                while (!go.load(std::memory_order_acquire)) {
                }
                const TransitionResult result =
                    (i % 2 == 0) ? job.MarkCompleted() : job.MarkCancelled();
                if (result == TransitionResult::Success) ++successes;
            });
        }
        go.store(true, std::memory_order_release);
        for (auto& thread : threads) thread.join();

        EXPECT_EQ(successes.load(), 1) << "exactly one thread may finalize a job";
        EXPECT_EQ(terminalCallbacks.load(), 1) << "a job must announce its terminal state once";
        EXPECT_TRUE(mediatool::jobs::IsTerminalState(job.State()));
    }
}
