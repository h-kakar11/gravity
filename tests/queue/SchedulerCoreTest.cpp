// SchedulerCore is pure and clock-free by construction, so every one of these tests is a
// deterministic sequence of calls with an explicit `now` -- no sleeps, no threads, no
// timing assumptions. Anything that turns out to need a sleep to pass is a design problem,
// not a test problem (spec section 62).

#include "core/queue/SchedulerCore.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "core/errors/MediaToolException.h"

using mediatool::errors::MediaToolException;
using mediatool::jobs::JobState;
using mediatool::jobs::JobType;
using mediatool::queue::HistoryScope;
using mediatool::queue::JobPriority;
using mediatool::queue::JobRecord;
using mediatool::queue::MoveDirection;
using mediatool::queue::QueueRunState;
using mediatool::queue::RetryPolicy;
using mediatool::queue::SchedulerCore;

namespace {

JobRecord MakeRecord(const std::string& id, JobPriority priority = JobPriority::Normal,
                     JobType type = JobType::Download) {
    JobRecord record;
    record.id = id;
    record.priority = priority;
    record.spec.type = type;
    return record;
}

// Drives a job all the way to Completed the way JobManager would.
void Complete(SchedulerCore& scheduler, const std::string& id, std::int64_t now) {
    scheduler.SetState(id, JobState::Running, now);
    scheduler.SetState(id, JobState::Completed, now);
}

// Fails a job, mirroring JobManager's Running -> Failed path.
void Fail(SchedulerCore& scheduler, const std::string& id, std::int64_t now) {
    scheduler.SetState(id, JobState::Running, now);
    scheduler.SetState(id, JobState::Failed, now);
}

class SchedulerCoreTest : public ::testing::Test {
protected:
    SchedulerCore MakeScheduler(std::size_t concurrency = 1, std::int64_t agingIntervalMs = 0) {
        SchedulerCore::Config config;
        config.maxConcurrency = concurrency;
        // Aging off by default: most tests are about priority/FIFO and would otherwise have
        // to reason about wall-clock drift. The fairness tests turn it on explicitly.
        config.agingIntervalMs = agingIntervalMs;
        return SchedulerCore(config);
    }

    // Every mutating test calls this: an operation that leaves the scheduler internally
    // inconsistent is a bug even if the assertion under test passed (spec section 45).
    static void ExpectConsistent(const SchedulerCore& scheduler) {
        const auto violations = scheduler.ValidateInvariants();
        EXPECT_TRUE(violations.empty()) << [&violations] {
            std::string joined;
            for (const auto& v : violations) joined += "\n  - " + v;
            return joined;
        }();
    }
};

}  // namespace

// --- ordering ----------------------------------------------------------------------------

TEST_F(SchedulerCoreTest, DispatchesInFifoOrderForEqualPriority) {
    auto scheduler = MakeScheduler(1);
    scheduler.Insert(MakeRecord("a"), 0);
    scheduler.Insert(MakeRecord("b"), 0);
    scheduler.Insert(MakeRecord("c"), 0);

    EXPECT_EQ(scheduler.SelectDispatchable(0), std::vector<std::string>{"a"});
    Complete(scheduler, "a", 1);
    EXPECT_EQ(scheduler.SelectDispatchable(1), std::vector<std::string>{"b"});
    Complete(scheduler, "b", 2);
    EXPECT_EQ(scheduler.SelectDispatchable(2), std::vector<std::string>{"c"});
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, HigherPriorityJumpsAheadOfEarlierQueuedWork) {
    auto scheduler = MakeScheduler(1);
    scheduler.Insert(MakeRecord("normal-1"), 0);
    scheduler.Insert(MakeRecord("normal-2"), 0);
    scheduler.Insert(MakeRecord("high", JobPriority::High), 0);
    scheduler.Insert(MakeRecord("low", JobPriority::Low), 0);

    EXPECT_EQ(scheduler.SelectDispatchable(0), std::vector<std::string>{"high"});
    Complete(scheduler, "high", 1);
    EXPECT_EQ(scheduler.SelectDispatchable(1), std::vector<std::string>{"normal-1"});
    Complete(scheduler, "normal-1", 2);
    EXPECT_EQ(scheduler.SelectDispatchable(2), std::vector<std::string>{"normal-2"});
    Complete(scheduler, "normal-2", 3);
    // LOW runs last even though it was queued before nothing else remained.
    EXPECT_EQ(scheduler.SelectDispatchable(3), std::vector<std::string>{"low"});
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, RaisingPriorityOfQueuedJobReordersIt) {
    auto scheduler = MakeScheduler(1);
    scheduler.Insert(MakeRecord("first"), 0);
    scheduler.Insert(MakeRecord("second"), 0);

    scheduler.SetPriority("second", JobPriority::High);
    EXPECT_EQ(scheduler.SelectDispatchable(0), std::vector<std::string>{"second"});
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, ChangingPriorityOfRunningJobDoesNotPreemptIt) {
    auto scheduler = MakeScheduler(1);
    scheduler.Insert(MakeRecord("running"), 0);
    // Dispatched and actually started BEFORE the high-priority job exists -- otherwise the
    // high one would legitimately have gone first and this would prove nothing.
    ASSERT_EQ(scheduler.SelectDispatchable(0), std::vector<std::string>{"running"});
    scheduler.SetState("running", JobState::Running, 0);
    scheduler.Insert(MakeRecord("waiting", JobPriority::High), 0);

    scheduler.SetPriority("running", JobPriority::Low);

    // The high-priority job still cannot start: the slot is taken and priority never kills
    // work that is already in flight (spec section 9).
    EXPECT_TRUE(scheduler.SelectDispatchable(1).empty());
    EXPECT_EQ(scheduler.Find("running")->state, JobState::Running);
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, PriorityCannotBeChangedOnFinishedJob) {
    auto scheduler = MakeScheduler(1);
    scheduler.Insert(MakeRecord("done"), 0);
    Complete(scheduler, "done", 1);
    EXPECT_THROW(scheduler.SetPriority("done", JobPriority::High), MediaToolException);
}

// --- reordering --------------------------------------------------------------------------

TEST_F(SchedulerCoreTest, MoveOperationsRepositionPendingJobs) {
    auto scheduler = MakeScheduler(1);
    for (const auto& id : {"a", "b", "c", "d"}) scheduler.Insert(MakeRecord(id), 0);

    scheduler.Move("d", MoveDirection::Top);
    EXPECT_EQ(scheduler.PendingOrder(), (std::vector<std::string>{"d", "a", "b", "c"}));

    scheduler.Move("d", MoveDirection::Bottom);
    EXPECT_EQ(scheduler.PendingOrder(), (std::vector<std::string>{"a", "b", "c", "d"}));

    scheduler.Move("c", MoveDirection::Up);
    EXPECT_EQ(scheduler.PendingOrder(), (std::vector<std::string>{"a", "c", "b", "d"}));

    scheduler.Move("a", MoveDirection::Down);
    EXPECT_EQ(scheduler.PendingOrder(), (std::vector<std::string>{"c", "a", "b", "d"}));
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, MoveAtBoundariesIsANoOpRatherThanAnError) {
    auto scheduler = MakeScheduler(1);
    scheduler.Insert(MakeRecord("a"), 0);
    scheduler.Insert(MakeRecord("b"), 0);

    scheduler.Move("a", MoveDirection::Up);      // already first
    scheduler.Move("b", MoveDirection::Down);    // already last
    EXPECT_EQ(scheduler.PendingOrder(), (std::vector<std::string>{"a", "b"}));
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, ReorderedPositionDecidesTiesAndSurvivesDispatch) {
    auto scheduler = MakeScheduler(1);
    for (const auto& id : {"a", "b", "c"}) scheduler.Insert(MakeRecord(id), 0);
    scheduler.Move("c", MoveDirection::Top);

    EXPECT_EQ(scheduler.SelectDispatchable(0), std::vector<std::string>{"c"});
    Complete(scheduler, "c", 1);
    EXPECT_EQ(scheduler.SelectDispatchable(1), std::vector<std::string>{"a"});
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, RunningAndFinishedJobsCannotBeMoved) {
    auto scheduler = MakeScheduler(1);
    scheduler.Insert(MakeRecord("running"), 0);
    scheduler.Insert(MakeRecord("done"), 0);
    ASSERT_FALSE(scheduler.SelectDispatchable(0).empty());
    scheduler.SetState("running", JobState::Running, 0);
    Complete(scheduler, "done", 1);

    EXPECT_THROW(scheduler.Move("running", MoveDirection::Top), MediaToolException);
    EXPECT_THROW(scheduler.Move("done", MoveDirection::Top), MediaToolException);
    EXPECT_THROW(scheduler.Move("nope", MoveDirection::Top), MediaToolException);
}

TEST_F(SchedulerCoreTest, RetryWaitJobsAreReorderable) {
    // Documented behaviour (spec section 10): a job waiting out a backoff IS pending, so it
    // can be reordered. Reordering changes its place among simultaneously-eligible jobs; it
    // does not make the backoff elapse any sooner.
    auto scheduler = MakeScheduler(1);
    scheduler.Insert(MakeRecord("retrying"), 0);
    scheduler.Insert(MakeRecord("fresh"), 0);
    ASSERT_FALSE(scheduler.SelectDispatchable(0).empty());
    Fail(scheduler, "retrying", 10);
    scheduler.ScheduleRetry("retrying", 1000, 10, "transient");

    scheduler.Move("retrying", MoveDirection::Top);
    EXPECT_EQ(scheduler.PendingOrder().front(), "retrying");

    // Still not eligible before its deadline, position notwithstanding.
    EXPECT_EQ(scheduler.SelectDispatchable(500), std::vector<std::string>{"fresh"});
    ExpectConsistent(scheduler);
}

// --- concurrency -------------------------------------------------------------------------

TEST_F(SchedulerCoreTest, NeverDispatchesBeyondConcurrencyLimit) {
    auto scheduler = MakeScheduler(2);
    for (int i = 0; i < 10; ++i) scheduler.Insert(MakeRecord("job-" + std::to_string(i)), 0);

    const auto first = scheduler.SelectDispatchable(0);
    EXPECT_EQ(first.size(), 2u);
    for (const auto& id : first) scheduler.SetState(id, JobState::Running, 0);

    // A second call while both slots are busy must hand out nothing at all.
    EXPECT_TRUE(scheduler.SelectDispatchable(0).empty());
    EXPECT_EQ(scheduler.RunningCount(), 2u);

    scheduler.SetState(first[0], JobState::Completed, 1);
    EXPECT_EQ(scheduler.SelectDispatchable(1).size(), 1u);
    EXPECT_EQ(scheduler.RunningCount(), 2u);
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, ConsecutiveSelectCallsNeverHandOutTheSameJobTwice) {
    // The double-dispatch guard: SelectDispatchable marks what it returns, so a caller that
    // calls it twice before starting anything still cannot run one job on two threads.
    auto scheduler = MakeScheduler(4);
    for (int i = 0; i < 4; ++i) scheduler.Insert(MakeRecord("job-" + std::to_string(i)), 0);

    const auto first = scheduler.SelectDispatchable(0);
    const auto second = scheduler.SelectDispatchable(0);

    EXPECT_EQ(first.size(), 4u);
    EXPECT_TRUE(second.empty());
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, LoweringConcurrencyDoesNotKillRunningJobsButBlocksNewOnes) {
    auto scheduler = MakeScheduler(3);
    for (int i = 0; i < 5; ++i) scheduler.Insert(MakeRecord("job-" + std::to_string(i)), 0);
    const auto running = scheduler.SelectDispatchable(0);
    ASSERT_EQ(running.size(), 3u);
    for (const auto& id : running) scheduler.SetState(id, JobState::Running, 0);

    scheduler.SetMaxConcurrency(1);

    EXPECT_EQ(scheduler.RunningCount(), 3u);           // in-flight work is left alone
    EXPECT_TRUE(scheduler.SelectDispatchable(1).empty());  // but nothing new starts
    scheduler.SetState(running[0], JobState::Completed, 2);
    scheduler.SetState(running[1], JobState::Completed, 2);
    EXPECT_TRUE(scheduler.SelectDispatchable(2).empty());  // still 1 running, limit is 1
    scheduler.SetState(running[2], JobState::Completed, 3);
    EXPECT_EQ(scheduler.SelectDispatchable(3).size(), 1u);
}

TEST_F(SchedulerCoreTest, ZeroConcurrencyIsCoercedToOne) {
    auto scheduler = MakeScheduler(2);
    scheduler.SetMaxConcurrency(0);
    EXPECT_EQ(scheduler.GetConfig().maxConcurrency, 1u);
}

// --- queue pause -------------------------------------------------------------------------

TEST_F(SchedulerCoreTest, PausedQueueStartsNothingNewButKeepsJobsQueued) {
    auto scheduler = MakeScheduler(2);
    scheduler.Insert(MakeRecord("a"), 0);
    scheduler.Insert(MakeRecord("b"), 0);

    scheduler.SetRunState(QueueRunState::Paused);
    EXPECT_TRUE(scheduler.SelectDispatchable(0).empty());
    EXPECT_EQ(scheduler.Find("a")->state, JobState::Queued);
    EXPECT_EQ(scheduler.PendingOrder().size(), 2u);

    scheduler.SetRunState(QueueRunState::Running);
    EXPECT_EQ(scheduler.SelectDispatchable(0).size(), 2u);
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, PausedQueueDoesNotStartADueRetry) {
    // Spec section 40: a retry that becomes eligible while the queue is paused must wait.
    auto scheduler = MakeScheduler(1);
    scheduler.Insert(MakeRecord("job"), 0);
    ASSERT_FALSE(scheduler.SelectDispatchable(0).empty());
    Fail(scheduler, "job", 10);
    scheduler.ScheduleRetry("job", 100, 10, "transient");

    scheduler.SetRunState(QueueRunState::Paused);
    EXPECT_TRUE(scheduler.SelectDispatchable(5'000).empty());
    EXPECT_EQ(scheduler.Find("job")->state, JobState::RetryWait);

    scheduler.SetRunState(QueueRunState::Running);
    EXPECT_EQ(scheduler.SelectDispatchable(5'000), std::vector<std::string>{"job"});
}

// --- retry -------------------------------------------------------------------------------

TEST_F(SchedulerCoreTest, RetryBecomesEligibleOnlyAfterItsBackoffElapses) {
    auto scheduler = MakeScheduler(1);
    scheduler.Insert(MakeRecord("job"), 0);
    ASSERT_FALSE(scheduler.SelectDispatchable(0).empty());
    Fail(scheduler, "job", 100);
    scheduler.ScheduleRetry("job", 2'000, 100, "network blip");

    EXPECT_EQ(scheduler.Find("job")->state, JobState::RetryWait);
    EXPECT_EQ(scheduler.Find("job")->attempt, 1);
    EXPECT_TRUE(scheduler.SelectDispatchable(2'099).empty());
    EXPECT_EQ(scheduler.SelectDispatchable(2'100), std::vector<std::string>{"job"});
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, RetryBudgetIsBounded) {
    auto scheduler = MakeScheduler(1);
    JobRecord record = MakeRecord("job");
    record.retryPolicy.maxRetries = 2;
    scheduler.Insert(std::move(record), 0);

    EXPECT_TRUE(scheduler.HasRetryBudget("job"));
    scheduler.ScheduleRetry("job", 0, 0, "1");
    EXPECT_TRUE(scheduler.HasRetryBudget("job"));
    scheduler.ScheduleRetry("job", 0, 0, "2");
    EXPECT_FALSE(scheduler.HasRetryBudget("job"));  // 2 attempts used, budget is 2
}

TEST_F(SchedulerCoreTest, NextWakeupReportsTheEarliestRetryDeadline) {
    auto scheduler = MakeScheduler(4);
    scheduler.Insert(MakeRecord("a"), 0);
    scheduler.Insert(MakeRecord("b"), 0);
    ASSERT_EQ(scheduler.SelectDispatchable(0).size(), 2u);
    Fail(scheduler, "a", 10);
    Fail(scheduler, "b", 10);
    scheduler.ScheduleRetry("a", 5'000, 10, "x");
    scheduler.ScheduleRetry("b", 1'000, 10, "x");

    EXPECT_EQ(scheduler.NextWakeupMs(10), 1'010);
    // Once a deadline has passed, "wake up now" is the honest answer.
    EXPECT_EQ(scheduler.NextWakeupMs(2'000), 2'000);
}

TEST_F(SchedulerCoreTest, NextWakeupIsEmptyWhenNothingIsTimeGated) {
    auto scheduler = MakeScheduler(1);
    scheduler.Insert(MakeRecord("a"), 0);
    EXPECT_FALSE(scheduler.NextWakeupMs(0).has_value());
}

TEST_F(SchedulerCoreTest, CancellingDuringRetryWaitPreventsTheAttempt) {
    // Spec section 39 case 4 / section 40: cancel during backoff, no retry may follow.
    auto scheduler = MakeScheduler(1);
    scheduler.Insert(MakeRecord("job"), 0);
    ASSERT_FALSE(scheduler.SelectDispatchable(0).empty());
    Fail(scheduler, "job", 10);
    scheduler.ScheduleRetry("job", 1'000, 10, "transient");

    scheduler.SetState("job", JobState::Cancelled, 20);

    EXPECT_TRUE(scheduler.SelectDispatchable(99'999).empty());
    EXPECT_EQ(scheduler.Find("job")->state, JobState::Cancelled);
    EXPECT_TRUE(scheduler.PendingOrder().empty());
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, RetryReentersPendingOrderAtTheTail) {
    auto scheduler = MakeScheduler(1);
    scheduler.Insert(MakeRecord("first"), 0);
    scheduler.Insert(MakeRecord("second"), 0);
    scheduler.Insert(MakeRecord("third"), 0);
    ASSERT_EQ(scheduler.SelectDispatchable(0), std::vector<std::string>{"first"});
    Fail(scheduler, "first", 10);
    scheduler.ScheduleRetry("first", 0, 10, "transient");

    // Work that was queued while "first" was running gets its turn before the retry.
    EXPECT_EQ(scheduler.PendingOrder(), (std::vector<std::string>{"second", "third", "first"}));
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, ManualRetrySkipsTheRemainingBackoff) {
    auto scheduler = MakeScheduler(1);
    scheduler.Insert(MakeRecord("job"), 0);
    ASSERT_FALSE(scheduler.SelectDispatchable(0).empty());
    Fail(scheduler, "job", 10);
    scheduler.ScheduleRetry("job", 60'000, 10, "transient");
    ASSERT_EQ(scheduler.Find("job")->nextRetryAtMs, 60'010);
    ASSERT_TRUE(scheduler.SelectDispatchable(20).empty());  // still waiting out the backoff

    scheduler.PrepareManualRetry("job", 20);

    EXPECT_EQ(scheduler.Find("job")->nextRetryAtMs, 20);  // due now
    EXPECT_EQ(scheduler.Find("job")->attempt, 2);
    EXPECT_EQ(scheduler.SelectDispatchable(20), std::vector<std::string>{"job"});
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, ManualRetryOfAFailedJobRequeuesIt) {
    auto scheduler = MakeScheduler(1);
    scheduler.Insert(MakeRecord("job"), 0);
    ASSERT_FALSE(scheduler.SelectDispatchable(0).empty());
    Fail(scheduler, "job", 10);
    ASSERT_TRUE(scheduler.PendingOrder().empty());

    scheduler.PrepareManualRetry("job", 20);

    EXPECT_EQ(scheduler.Find("job")->state, JobState::RetryWait);
    EXPECT_EQ(scheduler.PendingOrder(), std::vector<std::string>{"job"});
    EXPECT_EQ(scheduler.SelectDispatchable(20), std::vector<std::string>{"job"});
    ExpectConsistent(scheduler);
}

// --- dependencies ------------------------------------------------------------------------

TEST_F(SchedulerCoreTest, DependentStartsBlockedAndIsReleasedOnDependencySuccess) {
    auto scheduler = MakeScheduler(2);
    scheduler.Insert(MakeRecord("download"), 0);
    JobRecord convert = MakeRecord("convert", JobPriority::Normal, JobType::Conversion);
    convert.dependencies = {"download"};
    scheduler.Insert(std::move(convert), 0);

    EXPECT_EQ(scheduler.Find("convert")->state, JobState::Waiting);
    // Only the download is eligible, even though there are two free slots.
    EXPECT_EQ(scheduler.SelectDispatchable(0), std::vector<std::string>{"download"});

    Complete(scheduler, "download", 5);
    const auto transitions = scheduler.ResolveDependencies(5);
    ASSERT_EQ(transitions.size(), 1u);
    EXPECT_EQ(transitions[0].id, "convert");
    EXPECT_EQ(transitions[0].newState, JobState::Queued);
    EXPECT_EQ(scheduler.SelectDispatchable(5), std::vector<std::string>{"convert"});
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, DependencyFailureSkipsDependentRatherThanFailingIt) {
    auto scheduler = MakeScheduler(2);
    scheduler.Insert(MakeRecord("download"), 0);
    JobRecord convert = MakeRecord("convert");
    convert.dependencies = {"download"};
    scheduler.Insert(std::move(convert), 0);

    ASSERT_FALSE(scheduler.SelectDispatchable(0).empty());
    Fail(scheduler, "download", 5);

    const auto transitions = scheduler.ResolveDependencies(5);
    ASSERT_EQ(transitions.size(), 1u);
    EXPECT_EQ(transitions[0].newState, JobState::Skipped);
    ASSERT_TRUE(transitions[0].reason.has_value());
    EXPECT_EQ(transitions[0].reason->code, "E_DEPENDENCY_FAILED");
    // Skipped, not Failed: nothing about the conversion itself went wrong.
    EXPECT_EQ(scheduler.Find("convert")->state, JobState::Skipped);
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, DependencyCancellationSkipsDependent) {
    auto scheduler = MakeScheduler(2);
    scheduler.Insert(MakeRecord("parent"), 0);
    JobRecord child = MakeRecord("child");
    child.dependencies = {"parent"};
    scheduler.Insert(std::move(child), 0);

    scheduler.SetState("parent", JobState::Cancelled, 5);
    scheduler.ResolveDependencies(5);

    EXPECT_EQ(scheduler.Find("child")->state, JobState::Skipped);
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, SkipPropagatesDownAChain) {
    // Spec section 41: download -> convert -> compress, with the download failing.
    auto scheduler = MakeScheduler(3);
    scheduler.Insert(MakeRecord("download"), 0);
    JobRecord convert = MakeRecord("convert");
    convert.dependencies = {"download"};
    scheduler.Insert(std::move(convert), 0);
    JobRecord compress = MakeRecord("compress");
    compress.dependencies = {"convert"};
    scheduler.Insert(std::move(compress), 0);

    ASSERT_FALSE(scheduler.SelectDispatchable(0).empty());
    Fail(scheduler, "download", 5);

    scheduler.ResolveDependencies(5);   // download -> convert skipped
    scheduler.ResolveDependencies(6);   // convert  -> compress skipped

    EXPECT_EQ(scheduler.Find("convert")->state, JobState::Skipped);
    EXPECT_EQ(scheduler.Find("compress")->state, JobState::Skipped);
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, ChainRunsInDependencyOrderEvenWithSpareCapacity) {
    auto scheduler = MakeScheduler(8);
    scheduler.Insert(MakeRecord("download"), 0);
    JobRecord convert = MakeRecord("convert");
    convert.dependencies = {"download"};
    scheduler.Insert(std::move(convert), 0);
    JobRecord compress = MakeRecord("compress");
    compress.dependencies = {"convert"};
    scheduler.Insert(std::move(compress), 0);

    EXPECT_EQ(scheduler.SelectDispatchable(0), std::vector<std::string>{"download"});
    Complete(scheduler, "download", 1);
    scheduler.ResolveDependencies(1);
    EXPECT_EQ(scheduler.SelectDispatchable(1), std::vector<std::string>{"convert"});
    Complete(scheduler, "convert", 2);
    scheduler.ResolveDependencies(2);
    EXPECT_EQ(scheduler.SelectDispatchable(2), std::vector<std::string>{"compress"});
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, MultipleDependenciesAllMustCompleteFirst) {
    auto scheduler = MakeScheduler(4);
    scheduler.Insert(MakeRecord("a"), 0);
    scheduler.Insert(MakeRecord("b"), 0);
    JobRecord merged = MakeRecord("merged");
    merged.dependencies = {"a", "b"};
    scheduler.Insert(std::move(merged), 0);

    ASSERT_EQ(scheduler.SelectDispatchable(0).size(), 2u);
    Complete(scheduler, "a", 1);
    scheduler.ResolveDependencies(1);
    EXPECT_EQ(scheduler.Find("merged")->state, JobState::Waiting);

    Complete(scheduler, "b", 2);
    scheduler.ResolveDependencies(2);
    EXPECT_EQ(scheduler.Find("merged")->state, JobState::Queued);
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, DependencyCompletingBeforeDependentIsQueuedStartsItImmediately) {
    auto scheduler = MakeScheduler(2);
    scheduler.Insert(MakeRecord("done-already"), 0);
    Complete(scheduler, "done-already", 1);

    JobRecord dependent = MakeRecord("dependent");
    dependent.dependencies = {"done-already"};
    scheduler.Insert(std::move(dependent), 2);

    // Inserted straight into Queued -- there is nothing to wait for.
    EXPECT_EQ(scheduler.Find("dependent")->state, JobState::Queued);
    EXPECT_EQ(scheduler.SelectDispatchable(2), std::vector<std::string>{"dependent"});
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, RejectsUnknownSelfAndDuplicateDependencies) {
    auto scheduler = MakeScheduler(1);
    scheduler.Insert(MakeRecord("real"), 0);

    JobRecord unknown = MakeRecord("unknown-dep");
    unknown.dependencies = {"does-not-exist"};
    EXPECT_THROW(scheduler.Insert(std::move(unknown), 0), MediaToolException);

    JobRecord self = MakeRecord("self-dep");
    self.dependencies = {"self-dep"};
    EXPECT_THROW(scheduler.Insert(std::move(self), 0), MediaToolException);

    JobRecord repeated = MakeRecord("repeated-dep");
    repeated.dependencies = {"real", "real"};
    EXPECT_THROW(scheduler.Insert(std::move(repeated), 0), MediaToolException);

    // A rejected insert must leave nothing behind.
    EXPECT_FALSE(scheduler.Contains("unknown-dep"));
    EXPECT_FALSE(scheduler.Contains("self-dep"));
    EXPECT_FALSE(scheduler.Contains("repeated-dep"));
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, RejectsDependencyCycles) {
    auto scheduler = MakeScheduler(1);
    scheduler.Insert(MakeRecord("a"), 0);
    JobRecord b = MakeRecord("b");
    b.dependencies = {"a"};
    scheduler.Insert(std::move(b), 0);
    JobRecord c = MakeRecord("c");
    c.dependencies = {"b"};
    scheduler.Insert(std::move(c), 0);

    // Adding a -> c would close the loop a -> c -> b -> a. `a` already exists, so express
    // the cycle through a new job that depends on c and is depended on by... itself.
    JobRecord cycle = MakeRecord("cycle");
    cycle.dependencies = {"c", "cycle"};
    EXPECT_THROW(scheduler.Insert(std::move(cycle), 0), MediaToolException);
    ExpectConsistent(scheduler);
}

// --- duplicates --------------------------------------------------------------------------

TEST_F(SchedulerCoreTest, RejectsADuplicateOfAnActiveJob) {
    auto scheduler = MakeScheduler(1);
    JobRecord first = MakeRecord("first");
    first.duplicateKey = "DOWNLOAD|https://example.com/video";
    scheduler.Insert(std::move(first), 0);

    JobRecord second = MakeRecord("second");
    second.duplicateKey = "DOWNLOAD|https://example.com/video";
    EXPECT_THROW(scheduler.Insert(std::move(second), 0), MediaToolException);
    EXPECT_EQ(scheduler.FindActiveDuplicate("DOWNLOAD|https://example.com/video"), "first");
}

TEST_F(SchedulerCoreTest, AllowsTheSameRequestAgainOnceTheFirstHasFinished) {
    auto scheduler = MakeScheduler(1);
    JobRecord first = MakeRecord("first");
    first.duplicateKey = "k";
    scheduler.Insert(std::move(first), 0);
    Complete(scheduler, "first", 1);

    JobRecord second = MakeRecord("second");
    second.duplicateKey = "k";
    EXPECT_NO_THROW(scheduler.Insert(std::move(second), 2));
    EXPECT_FALSE(scheduler.FindActiveDuplicate("k").has_value() &&
                 *scheduler.FindActiveDuplicate("k") == "first");
}

TEST_F(SchedulerCoreTest, DifferentParametersAreNotDuplicates) {
    auto scheduler = MakeScheduler(1);
    JobRecord first = MakeRecord("first");
    first.duplicateKey = "DOWNLOAD|{\"quality\":\"BEST\"}";
    scheduler.Insert(std::move(first), 0);

    JobRecord second = MakeRecord("second");
    second.duplicateKey = "DOWNLOAD|{\"quality\":\"AUDIO_ONLY\"}";
    EXPECT_NO_THROW(scheduler.Insert(std::move(second), 0));
}

TEST_F(SchedulerCoreTest, EmptyDuplicateKeyDisablesTheCheck) {
    auto scheduler = MakeScheduler(1);
    scheduler.Insert(MakeRecord("a"), 0);
    EXPECT_NO_THROW(scheduler.Insert(MakeRecord("b"), 0));
    EXPECT_FALSE(scheduler.FindActiveDuplicate("").has_value());
}

TEST_F(SchedulerCoreTest, RejectsDuplicateJobId) {
    auto scheduler = MakeScheduler(1);
    scheduler.Insert(MakeRecord("same"), 0);
    EXPECT_THROW(scheduler.Insert(MakeRecord("same"), 0), MediaToolException);
}

// --- fairness ----------------------------------------------------------------------------

TEST_F(SchedulerCoreTest, AgingEventuallyLetsANormalJobBeatFreshHighPriorityWork) {
    // Spec section 44: a steady stream of HIGH work must not starve a NORMAL job forever.
    auto scheduler = MakeScheduler(1, /*agingIntervalMs=*/1'000);
    scheduler.Insert(MakeRecord("starved"), 0);

    std::int64_t now = 0;
    bool starvedRan = false;
    for (int i = 0; i < 30 && !starvedRan; ++i) {
        scheduler.Insert(MakeRecord("high-" + std::to_string(i), JobPriority::High), now);
        const auto dispatched = scheduler.SelectDispatchable(now);
        ASSERT_EQ(dispatched.size(), 1u);
        if (dispatched[0] == "starved") {
            starvedRan = true;
            break;
        }
        Complete(scheduler, dispatched[0], now);
        now += 1'000;
    }

    EXPECT_TRUE(starvedRan) << "NORMAL job never ran despite continuous HIGH arrivals";
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, AgingIsBoundedSoPriorityStillMeansSomething) {
    auto scheduler = MakeScheduler(1, /*agingIntervalMs=*/1'000);
    SchedulerCore::Config config = scheduler.GetConfig();
    config.maxAgingBoost = 5;  // less than the 10-rank gap between tiers
    scheduler = SchedulerCore(config);

    scheduler.Insert(MakeRecord("low", JobPriority::Low), 0);
    // Even after a very long wait, +5 cannot lift LOW (0) past a fresh HIGH (20).
    scheduler.Insert(MakeRecord("high", JobPriority::High), 1'000'000);
    EXPECT_EQ(scheduler.SelectDispatchable(1'000'000), std::vector<std::string>{"high"});
}

TEST_F(SchedulerCoreTest, AgingDisabledMeansStrictPriority) {
    auto scheduler = MakeScheduler(1, /*agingIntervalMs=*/0);
    scheduler.Insert(MakeRecord("normal"), 0);
    scheduler.Insert(MakeRecord("high", JobPriority::High), 1'000'000);
    EXPECT_EQ(scheduler.SelectDispatchable(9'999'999), std::vector<std::string>{"high"});
}

TEST_F(SchedulerCoreTest, RetryDoesNotInheritAgingFromTheOriginalWait) {
    auto scheduler = MakeScheduler(1, /*agingIntervalMs=*/1'000);
    scheduler.Insert(MakeRecord("old"), 0);
    ASSERT_FALSE(scheduler.SelectDispatchable(0).empty());
    Fail(scheduler, "old", 100'000);
    scheduler.ScheduleRetry("old", 0, 100'000, "transient");

    // Aging restarts at the retry, so a job that has already had a turn does not come back
    // pre-aged and immediately outrank everything.
    EXPECT_EQ(scheduler.Find("old")->pendingSinceMs, 100'000);
}

// --- history -----------------------------------------------------------------------------

TEST_F(SchedulerCoreTest, ClearHistoryRemovesOnlyTheRequestedTerminalScope) {
    auto scheduler = MakeScheduler(4);
    for (const auto& id : {"done", "failed", "cancelled", "queued"})
        scheduler.Insert(MakeRecord(id), 0);
    ASSERT_FALSE(scheduler.SelectDispatchable(0).empty());
    Complete(scheduler, "done", 1);
    Fail(scheduler, "failed", 1);
    scheduler.SetState("cancelled", JobState::Cancelled, 1);

    const auto removed = scheduler.ClearHistory(HistoryScope::Completed);
    EXPECT_EQ(removed, std::vector<std::string>{"done"});
    EXPECT_FALSE(scheduler.Contains("done"));
    EXPECT_TRUE(scheduler.Contains("failed"));
    EXPECT_TRUE(scheduler.Contains("cancelled"));
    EXPECT_TRUE(scheduler.Contains("queued"));
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, ClearAllHistoryNeverRemovesLiveJobs) {
    auto scheduler = MakeScheduler(2);
    scheduler.Insert(MakeRecord("running"), 0);
    scheduler.Insert(MakeRecord("queued"), 0);
    scheduler.Insert(MakeRecord("done"), 0);
    ASSERT_FALSE(scheduler.SelectDispatchable(0).empty());
    scheduler.SetState("running", JobState::Running, 0);
    Complete(scheduler, "done", 1);

    scheduler.ClearHistory(HistoryScope::All);

    EXPECT_TRUE(scheduler.Contains("running"));
    EXPECT_TRUE(scheduler.Contains("queued"));
    EXPECT_FALSE(scheduler.Contains("done"));
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, HistoryIsBoundedAndEvictsSuccessesBeforeFailures) {
    SchedulerCore::Config config;
    config.maxConcurrency = 1;
    config.agingIntervalMs = 0;
    config.historyLimit = 3;
    SchedulerCore scheduler(config);

    scheduler.Insert(MakeRecord("failure"), 0);
    ASSERT_FALSE(scheduler.SelectDispatchable(0).empty());
    Fail(scheduler, "failure", 1);

    for (int i = 0; i < 5; ++i) {
        const std::string id = "ok-" + std::to_string(i);
        scheduler.Insert(MakeRecord(id), 2);
        ASSERT_FALSE(scheduler.SelectDispatchable(2).empty());
        Complete(scheduler, id, 3);
    }

    EXPECT_EQ(scheduler.Stats().total, 3);
    // The failure survives -- it is the entry a user actually needs to look at later.
    EXPECT_TRUE(scheduler.Contains("failure"));
    EXPECT_TRUE(scheduler.Contains("ok-4"));
    EXPECT_FALSE(scheduler.Contains("ok-0"));
    ExpectConsistent(scheduler);
}

TEST_F(SchedulerCoreTest, RemoveRejectsLiveJobsAndAcceptsFinishedOnes) {
    auto scheduler = MakeScheduler(1);
    scheduler.Insert(MakeRecord("queued"), 0);
    scheduler.Insert(MakeRecord("done"), 0);
    ASSERT_FALSE(scheduler.SelectDispatchable(0).empty());
    Complete(scheduler, "queued", 1);

    EXPECT_THROW(scheduler.Remove("done"), MediaToolException);  // still Queued
    EXPECT_NO_THROW(scheduler.Remove("queued"));                 // this one finished
    EXPECT_THROW(scheduler.Remove("nope"), MediaToolException);
}

// --- statistics & snapshot ----------------------------------------------------------------

TEST_F(SchedulerCoreTest, StatisticsCountEveryState) {
    auto scheduler = MakeScheduler(2);
    for (const auto& id : {"run", "done", "fail", "cancel", "queue"})
        scheduler.Insert(MakeRecord(id), 0);
    JobRecord blocked = MakeRecord("blocked");
    blocked.dependencies = {"fail"};
    scheduler.Insert(std::move(blocked), 0);

    ASSERT_FALSE(scheduler.SelectDispatchable(0).empty());
    scheduler.SetState("run", JobState::Running, 0);
    Complete(scheduler, "done", 1);
    Fail(scheduler, "fail", 1);
    scheduler.SetState("cancel", JobState::Cancelled, 1);

    const auto stats = scheduler.Stats();
    EXPECT_EQ(stats.running, 1);
    EXPECT_EQ(stats.completed, 1);
    EXPECT_EQ(stats.failed, 1);
    EXPECT_EQ(stats.cancelled, 1);
    EXPECT_EQ(stats.waiting, 1);
    EXPECT_EQ(stats.queued, 1);
    EXPECT_EQ(stats.total, 6);
}

TEST_F(SchedulerCoreTest, SnapshotListsPendingJobsInQueueOrderFirst) {
    auto scheduler = MakeScheduler(1);
    for (const auto& id : {"a", "b", "c"}) scheduler.Insert(MakeRecord(id), 0);
    ASSERT_FALSE(scheduler.SelectDispatchable(0).empty());  // "a" starts
    scheduler.Move("c", MoveDirection::Top);

    const auto snapshot = scheduler.Snapshot();
    ASSERT_EQ(snapshot.size(), 3u);
    EXPECT_EQ(snapshot[0].id, "c");   // pending, moved to top
    EXPECT_EQ(snapshot[1].id, "b");   // pending
    EXPECT_EQ(snapshot[2].id, "a");   // running, so after the pending block
}
