#include "core/jobs/SchedulerCore.h"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/errors/MediaToolException.h"

namespace mediatool::jobs {
namespace {

using errors::MediaToolException;

// Not a single thread in this file. That is the point of extracting the scheduler: what
// used to require starting a worker pool and hoping for an interesting interleaving is now
// a deterministic function call.
class SchedulerCoreTest : public ::testing::Test {
protected:
    void SubmitJob(const std::string& id, int priority = 0, std::vector<JobId> dependsOn = {}) {
        scheduler_.Submit({id, priority, std::move(dependsOn)});
    }

    // Drains the scheduler as a worker pool of size 1 would, completing each job before
    // asking for the next -- the order this produces is the schedule.
    std::vector<JobId> RunToCompletion() {
        std::vector<JobId> order;
        while (const std::optional<JobId> next = scheduler_.TakeNextEligible()) {
            order.push_back(*next);
            scheduler_.RecordTerminal(*next, JobState::Completed);
        }
        return order;
    }

    SchedulerCore scheduler_;
};

TEST_F(SchedulerCoreTest, EmptySchedulerHasNothingEligible) {
    EXPECT_FALSE(scheduler_.HasEligible());
    EXPECT_FALSE(scheduler_.TakeNextEligible().has_value());
    EXPECT_EQ(scheduler_.PendingCount(), 0u);
}

TEST_F(SchedulerCoreTest, EqualPrioritiesKeepSubmissionOrder) {
    SubmitJob("a");
    SubmitJob("b");
    SubmitJob("c");
    EXPECT_EQ(RunToCompletion(), (std::vector<JobId>{"a", "b", "c"}));
}

TEST_F(SchedulerCoreTest, HigherPriorityRunsFirstAndTiesStayFifo) {
    SubmitJob("low-1", 0);
    SubmitJob("high-1", 10);
    SubmitJob("low-2", 0);
    SubmitJob("high-2", 10);
    SubmitJob("urgent", 100);
    SubmitJob("negative", -5);

    EXPECT_EQ(RunToCompletion(), (std::vector<JobId>{"urgent", "high-1", "high-2", "low-1", "low-2",
                                                      "negative"}));
}

TEST_F(SchedulerCoreTest, PriorityAppliesToJobsSubmittedWhileOthersWait) {
    SubmitJob("first", 0);
    SubmitJob("second", 0);
    // Taking "first" leaves "second" pending; a later high-priority arrival must overtake it.
    ASSERT_EQ(scheduler_.TakeNextEligible(), std::optional<JobId>("first"));
    SubmitJob("jumped-the-queue", 50);
    EXPECT_EQ(scheduler_.PendingOrder(), (std::vector<JobId>{"jumped-the-queue", "second"}));
}

TEST_F(SchedulerCoreTest, ADependentWaitsUntilItsDependencyCompletes) {
    SubmitJob("download");
    SubmitJob("convert", 0, {"download"});

    ASSERT_EQ(scheduler_.TakeNextEligible(), std::optional<JobId>("download"));
    EXPECT_FALSE(scheduler_.HasEligible()) << "convert must not start before download finishes";
    EXPECT_EQ(scheduler_.PendingCount(), 1u);

    scheduler_.RecordTerminal("download", JobState::Completed);
    EXPECT_TRUE(scheduler_.HasEligible());
    EXPECT_EQ(scheduler_.TakeNextEligible(), std::optional<JobId>("convert"));
}

TEST_F(SchedulerCoreTest, TheExampleFromTheIssueSchedulesAsCPriorityThenAThenB) {
    // Submit A(priority 10), B(depends on A, priority 1), C(priority 5).
    // Expected: C, A, B -- C outranks nothing it must wait for, A outranks B, and B cannot
    // run until A completes regardless of what any priority says.
    SubmitJob("A", 10);
    SubmitJob("B", 1, {"A"});
    SubmitJob("C", 5);

    EXPECT_EQ(RunToCompletion(), (std::vector<JobId>{"A", "C", "B"}));
}

TEST_F(SchedulerCoreTest, AnUnmetDependencyIsSkippedNotBlocking) {
    // A low-priority job whose dependencies are met runs ahead of a high-priority job that
    // is still waiting: head-of-line blocking would idle the whole pool on one stalled
    // chain.
    SubmitJob("slow", 0);
    SubmitJob("high-but-waiting", 100, {"slow"});
    SubmitJob("low-but-ready", 1);

    // Scheduling order is [high-but-waiting(100), low-but-ready(1), slow(0)], but the
    // highest-priority entry is waiting on "slow", so it is passed over rather than
    // stalling the queue behind it.
    EXPECT_EQ(scheduler_.TakeNextEligible(), std::optional<JobId>("low-but-ready"));
    EXPECT_EQ(scheduler_.TakeNextEligible(), std::optional<JobId>("slow"));
    EXPECT_FALSE(scheduler_.HasEligible());

    scheduler_.RecordTerminal("slow", JobState::Completed);
    EXPECT_EQ(scheduler_.TakeNextEligible(), std::optional<JobId>("high-but-waiting"));
}

TEST_F(SchedulerCoreTest, DependenciesOnSeveralJobsAllHaveToComplete) {
    SubmitJob("a");
    SubmitJob("b");
    SubmitJob("merge", 0, {"a", "b"});

    ASSERT_TRUE(scheduler_.TakeNextEligible().has_value());  // a
    ASSERT_TRUE(scheduler_.TakeNextEligible().has_value());  // b
    scheduler_.RecordTerminal("a", JobState::Completed);
    EXPECT_FALSE(scheduler_.HasEligible()) << "one of two dependencies is not enough";
    scheduler_.RecordTerminal("b", JobState::Completed);
    EXPECT_EQ(scheduler_.TakeNextEligible(), std::optional<JobId>("merge"));
}

TEST_F(SchedulerCoreTest, AFailedDependencyReportsItsDependentsAsUnrunnable) {
    SubmitJob("download");
    SubmitJob("convert", 0, {"download"});
    ASSERT_TRUE(scheduler_.TakeNextEligible().has_value());

    const std::vector<JobId> unrunnable = scheduler_.RecordTerminal("download", JobState::Failed);
    EXPECT_EQ(unrunnable, (std::vector<JobId>{"convert"}));
}

TEST_F(SchedulerCoreTest, ACancelledDependencyAlsoStrandsItsDependents) {
    SubmitJob("download");
    SubmitJob("convert", 0, {"download"});
    ASSERT_TRUE(scheduler_.TakeNextEligible().has_value());
    EXPECT_EQ(scheduler_.RecordTerminal("download", JobState::Cancelled),
              (std::vector<JobId>{"convert"}));
}

TEST_F(SchedulerCoreTest, DependencyFailurePropagatesOneLinkAtATime) {
    // A -> B -> C. Failing A strands B directly; C is stranded when B's own cancellation is
    // reported, which is how the chain unwinds through the caller rather than in a
    // traversal here.
    SubmitJob("A");
    SubmitJob("B", 0, {"A"});
    SubmitJob("C", 0, {"B"});
    ASSERT_TRUE(scheduler_.TakeNextEligible().has_value());  // A

    const std::vector<JobId> firstWave = scheduler_.RecordTerminal("A", JobState::Failed);
    EXPECT_EQ(firstWave, (std::vector<JobId>{"B"}));

    const std::vector<JobId> secondWave = scheduler_.RecordTerminal("B", JobState::Cancelled);
    EXPECT_EQ(secondWave, (std::vector<JobId>{"C"}));
    EXPECT_EQ(scheduler_.RecordTerminal("C", JobState::Cancelled), std::vector<JobId>{});
    EXPECT_EQ(scheduler_.PendingCount(), 0u);
}

TEST_F(SchedulerCoreTest, RecordingTheSameTerminalOutcomeTwiceIsANoOp) {
    SubmitJob("a");
    SubmitJob("b", 0, {"a"});
    ASSERT_TRUE(scheduler_.TakeNextEligible().has_value());

    EXPECT_EQ(scheduler_.RecordTerminal("a", JobState::Failed), (std::vector<JobId>{"b"}));
    EXPECT_EQ(scheduler_.RecordTerminal("a", JobState::Failed), std::vector<JobId>{})
        << "the second report must not strand the same dependent again";
}

TEST_F(SchedulerCoreTest, ADuplicateDependencyEdgeReportsTheDependentOnce) {
    SubmitJob("a");
    SubmitJob("b", 0, {"a", "a"});
    ASSERT_TRUE(scheduler_.TakeNextEligible().has_value());
    EXPECT_EQ(scheduler_.RecordTerminal("a", JobState::Failed), (std::vector<JobId>{"b"}));
}

// --- runAfter: ordering without failure coupling (issue #41) ------------------------------
//
// The distinction these pin down is the whole reason runAfter exists: a playlist chained
// with dependsOn loses every remaining entry to one unavailable video.

TEST_F(SchedulerCoreTest, RunAfterHoldsAJobBackUntilItsPredecessorFinishes) {
    scheduler_.Submit({"a", 0, {}, {}});
    scheduler_.Submit({"b", 0, {}, /*runAfter=*/{"a"}});

    EXPECT_EQ(scheduler_.TakeNextEligible(), std::optional<JobId>("a"));
    // "b" is not eligible while "a" is still running -- that is the sequencing.
    EXPECT_FALSE(scheduler_.TakeNextEligible().has_value());

    scheduler_.RecordTerminal("a", JobState::Completed);
    EXPECT_EQ(scheduler_.TakeNextEligible(), std::optional<JobId>("b"));
}

TEST_F(SchedulerCoreTest, RunAfterReleasesTheNextJobEvenWhenItsPredecessorFailed) {
    scheduler_.Submit({"a", 0, {}, {}});
    scheduler_.Submit({"b", 0, {}, /*runAfter=*/{"a"}});

    ASSERT_EQ(scheduler_.TakeNextEligible(), std::optional<JobId>("a"));
    const std::vector<JobId> stranded = scheduler_.RecordTerminal("a", JobState::Failed);

    // Nothing is cancelled: a sequencing edge carries no failure semantics at all.
    EXPECT_TRUE(stranded.empty());
    EXPECT_EQ(scheduler_.TakeNextEligible(), std::optional<JobId>("b"));
}

TEST_F(SchedulerCoreTest, RunAfterReleasesTheNextJobWhenItsPredecessorWasCancelled) {
    scheduler_.Submit({"a", 0, {}, {}});
    scheduler_.Submit({"b", 0, {}, /*runAfter=*/{"a"}});

    ASSERT_EQ(scheduler_.TakeNextEligible(), std::optional<JobId>("a"));
    EXPECT_TRUE(scheduler_.RecordTerminal("a", JobState::Cancelled).empty());
    EXPECT_EQ(scheduler_.TakeNextEligible(), std::optional<JobId>("b"));
}

TEST_F(SchedulerCoreTest, DependsOnStillCancelsDependentsWhenItsDependencyFails) {
    // The contrast case, kept adjacent on purpose: runAfter must not have weakened the
    // workflow guarantee that dependsOn exists to provide.
    scheduler_.Submit({"a", 0, {}, {}});
    scheduler_.Submit({"b", 0, /*dependsOn=*/{"a"}, {}});

    ASSERT_EQ(scheduler_.TakeNextEligible(), std::optional<JobId>("a"));
    EXPECT_EQ(scheduler_.RecordTerminal("a", JobState::Failed), (std::vector<JobId>{"b"}));
}

TEST_F(SchedulerCoreTest, AChainOfRunAfterJobsRunsInOrderAndSurvivesAFailureMidway) {
    // A three-entry playlist whose middle video is unavailable: the other two still run,
    // and still in order.
    scheduler_.Submit({"one", 0, {}, {}});
    scheduler_.Submit({"two", 0, {}, /*runAfter=*/{"one"}});
    scheduler_.Submit({"three", 0, {}, /*runAfter=*/{"two"}});

    std::vector<JobId> order;
    while (const std::optional<JobId> next = scheduler_.TakeNextEligible()) {
        order.push_back(*next);
        scheduler_.RecordTerminal(*next, *next == "two" ? JobState::Failed : JobState::Completed);
    }
    EXPECT_EQ(order, (std::vector<JobId>{"one", "two", "three"}));
}

TEST_F(SchedulerCoreTest, RunAfterOnlyOneJobRunsAtATimeEvenWithSpareCapacity) {
    // TakeNextEligible is the scheduler's whole answer to "what may start now", so a second
    // call returning nothing IS the concurrency guarantee, whatever the pool size is.
    scheduler_.Submit({"one", 0, {}, {}});
    scheduler_.Submit({"two", 0, {}, /*runAfter=*/{"one"}});
    scheduler_.Submit({"three", 0, {}, /*runAfter=*/{"two"}});

    ASSERT_TRUE(scheduler_.TakeNextEligible().has_value());
    EXPECT_FALSE(scheduler_.TakeNextEligible().has_value());
    EXPECT_FALSE(scheduler_.HasEligible());
}

TEST_F(SchedulerCoreTest, RunAfterAnAlreadyFailedJobIsAcceptedRatherThanRejected) {
    // dependsOn refuses this (the edge could never be satisfied); runAfter must accept it,
    // because a finished predecessor is a *satisfied* sequencing edge. Queueing the tail of
    // a playlist after an early entry has already failed depends on this.
    scheduler_.Submit({"a", 0, {}, {}});
    ASSERT_TRUE(scheduler_.TakeNextEligible().has_value());
    scheduler_.RecordTerminal("a", JobState::Failed);

    scheduler_.Submit({"b", 0, {}, /*runAfter=*/{"a"}});
    EXPECT_EQ(scheduler_.TakeNextEligible(), std::optional<JobId>("b"));
}

TEST_F(SchedulerCoreTest, SelfRunAfterIsRejected) {
    try {
        scheduler_.Submit({"a", 0, {}, /*runAfter=*/{"a"}});
        FAIL() << "a job running after itself must be rejected";
    } catch (const MediaToolException& e) {
        EXPECT_EQ(e.Info().code, "E_INVALID_DEPENDENCY");
    }
}

TEST_F(SchedulerCoreTest, RunAfterAnUnknownJobIsRejected) {
    // Same guard as dependsOn, and for the same reason: an edge may only point backwards in
    // submission order, which is what makes a cycle impossible rather than merely detected.
    try {
        scheduler_.Submit({"a", 0, {}, /*runAfter=*/{"nope"}});
        FAIL() << "runAfter naming an unsubmitted job must be rejected";
    } catch (const MediaToolException& e) {
        EXPECT_EQ(e.Info().code, "E_INVALID_DEPENDENCY");
    }
}

// --- rejected submissions ---------------------------------------------------------------

TEST_F(SchedulerCoreTest, SelfDependencyIsRejected) {
    try {
        scheduler_.Submit({"a", 0, {"a"}});
        FAIL() << "a job depending on itself must be rejected";
    } catch (const MediaToolException& e) {
        EXPECT_EQ(e.Info().code, "E_INVALID_DEPENDENCY");
    }
    EXPECT_FALSE(scheduler_.Knows("a")) << "a rejected submission must leave no trace";
}

TEST_F(SchedulerCoreTest, DependencyOnAnUnknownJobIsRejected) {
    try {
        scheduler_.Submit({"b", 0, {"a-that-was-never-submitted"}});
        FAIL() << "a dependency on an unknown job must be rejected";
    } catch (const MediaToolException& e) {
        EXPECT_EQ(e.Info().code, "E_INVALID_DEPENDENCY");
    }
    EXPECT_EQ(scheduler_.PendingCount(), 0u);
}

TEST_F(SchedulerCoreTest, ACycleCannotBeConstructed) {
    // The cycle guard is structural: an edge can only point at a job that already exists,
    // so building A -> B -> A fails at the point where A would have to name B.
    SubmitJob("A");
    SubmitJob("B", 0, {"A"});  // B waits on A: fine, A already existed

    // Now try to close the loop by making A wait on B. A is already submitted, so the only
    // way to express this is a resubmission -- which is refused as a duplicate, and would
    // be refused as an invalid dependency even if it were not.
    try {
        scheduler_.Submit({"A", 0, {"B"}});
        FAIL() << "closing a dependency cycle must be impossible";
    } catch (const MediaToolException& e) {
        EXPECT_EQ(e.Info().code, "E_DUPLICATE_JOB");
    }

    // And a fresh job can only ever point backwards, which is what makes the graph a DAG.
    SubmitJob("C", 0, {"A", "B"});
    EXPECT_TRUE(scheduler_.Knows("C"));
}

TEST_F(SchedulerCoreTest, DependingOnAnAlreadyFailedJobIsRejectedAtSubmission) {
    SubmitJob("a");
    ASSERT_TRUE(scheduler_.TakeNextEligible().has_value());
    scheduler_.RecordTerminal("a", JobState::Failed);

    try {
        scheduler_.Submit({"b", 0, {"a"}});
        FAIL() << "a job that could never run must be refused, not accepted and cancelled";
    } catch (const MediaToolException& e) {
        EXPECT_EQ(e.Info().code, "E_INVALID_DEPENDENCY");
        EXPECT_NE(e.Info().details.find("FAILED"), std::string::npos) << e.Info().details;
    }
}

TEST_F(SchedulerCoreTest, DependingOnAnAlreadyCompletedJobIsFineAndImmediatelyEligible) {
    SubmitJob("a");
    ASSERT_TRUE(scheduler_.TakeNextEligible().has_value());
    scheduler_.RecordTerminal("a", JobState::Completed);

    SubmitJob("b", 0, {"a"});
    EXPECT_EQ(scheduler_.TakeNextEligible(), std::optional<JobId>("b"));
}

TEST_F(SchedulerCoreTest, DuplicateSubmissionIsRejected) {
    SubmitJob("a");
    try {
        scheduler_.Submit({"a", 0, {}});
        FAIL() << "the same job id must not be schedulable twice";
    } catch (const MediaToolException& e) {
        EXPECT_EQ(e.Info().code, "E_DUPLICATE_JOB");
    }
    EXPECT_EQ(scheduler_.PendingCount(), 1u) << "the original submission is untouched";
}

TEST_F(SchedulerCoreTest, APartiallyInvalidDependencyListLeavesNothingBehind) {
    SubmitJob("good");
    EXPECT_THROW(scheduler_.Submit({"b", 0, {"good", "missing"}}), MediaToolException);
    // The valid edge from the rejected submission must not have been recorded: if it had,
    // completing "good" would report a dependent that does not exist.
    ASSERT_TRUE(scheduler_.TakeNextEligible().has_value());
    EXPECT_EQ(scheduler_.RecordTerminal("good", JobState::Failed), std::vector<JobId>{});
}

// --- retry, removal, shutdown -----------------------------------------------------------

TEST_F(SchedulerCoreTest, RequeueingAFailedJobPutsItBackAtTheEndOfItsPriorityBand) {
    SubmitJob("a");
    SubmitJob("b");
    ASSERT_TRUE(scheduler_.TakeNextEligible().has_value());  // a
    scheduler_.RecordTerminal("a", JobState::Failed);

    scheduler_.Requeue("a", 0);
    EXPECT_EQ(scheduler_.PendingOrder(), (std::vector<JobId>{"b", "a"}))
        << "a retried job waits its turn behind jobs that have not run yet";
}

TEST_F(SchedulerCoreTest, RequeueingCanRaisePriority) {
    SubmitJob("a");
    SubmitJob("b");
    ASSERT_TRUE(scheduler_.TakeNextEligible().has_value());
    scheduler_.RecordTerminal("a", JobState::Failed);

    scheduler_.Requeue("a", 100);
    EXPECT_EQ(scheduler_.PendingOrder(), (std::vector<JobId>{"a", "b"}));
}

TEST_F(SchedulerCoreTest, RequeueingAnUnknownJobThrows) {
    EXPECT_THROW(scheduler_.Requeue("nope", 0), MediaToolException);
}

TEST_F(SchedulerCoreTest, ForgettingAJobThatSomethingIsWaitingOnIsRefused) {
    SubmitJob("a");
    SubmitJob("b", 0, {"a"});
    ASSERT_TRUE(scheduler_.TakeNextEligible().has_value());
    scheduler_.RecordTerminal("a", JobState::Completed);

    try {
        scheduler_.Forget("a");
        FAIL() << "forgetting a dependency would leave its dependent waiting forever";
    } catch (const MediaToolException& e) {
        EXPECT_EQ(e.Info().code, "E_JOB_HAS_DEPENDENTS");
    }

    // Once the dependent has run, the dependency can be forgotten.
    ASSERT_EQ(scheduler_.TakeNextEligible(), std::optional<JobId>("b"));
    scheduler_.RecordTerminal("b", JobState::Completed);
    EXPECT_NO_THROW(scheduler_.Forget("a"));
    EXPECT_FALSE(scheduler_.Knows("a"));
}

TEST_F(SchedulerCoreTest, ForgettingRemovesTheJobFromBothDirectionsOfTheGraph) {
    SubmitJob("a");
    SubmitJob("b", 0, {"a"});
    ASSERT_TRUE(scheduler_.TakeNextEligible().has_value());
    scheduler_.RecordTerminal("a", JobState::Completed);
    ASSERT_TRUE(scheduler_.TakeNextEligible().has_value());
    scheduler_.RecordTerminal("b", JobState::Completed);

    scheduler_.Forget("b");
    // "a" no longer has "b" as a dependent, so a (hypothetical) later failure report for it
    // must not resurrect the forgotten id.
    EXPECT_EQ(scheduler_.RecordTerminal("a", JobState::Failed), std::vector<JobId>{});
    EXPECT_NO_THROW(scheduler_.Forget("a"));
    EXPECT_NO_THROW(scheduler_.Forget("never-existed"));
}

TEST_F(SchedulerCoreTest, TakeAllPendingEmptiesTheQueueInSchedulingOrder) {
    SubmitJob("low", 0);
    SubmitJob("high", 10);
    SubmitJob("running-already", 0);
    ASSERT_EQ(scheduler_.TakeNextEligible(), std::optional<JobId>("high"));

    const std::vector<JobId> taken = scheduler_.TakeAllPending();
    EXPECT_EQ(taken, (std::vector<JobId>{"low", "running-already"}));
    EXPECT_EQ(scheduler_.PendingCount(), 0u);
    EXPECT_FALSE(scheduler_.HasEligible());
    // The job already running is untouched: shutdown waits for in-flight work.
    EXPECT_TRUE(scheduler_.Knows("high"));
}

TEST_F(SchedulerCoreTest, ALargeQueueStaysInStrictSchedulingOrder) {
    // 10,002 jobs across three priority bands, submitted interleaved. Exercises the
    // ordering invariant at a size well past anything a user reaches by hand, and would
    // take minutes rather than milliseconds if the pending set went back to a structure
    // whose insert scans what is already queued.
    constexpr int kPerBand = 10000 / 3;
    for (int i = 0; i < kPerBand; ++i) {
        SubmitJob("low-" + std::to_string(i), 0);
        SubmitJob("mid-" + std::to_string(i), 5);
        SubmitJob("high-" + std::to_string(i), 9);
    }

    const std::vector<JobId> order = RunToCompletion();
    ASSERT_EQ(order.size(), static_cast<size_t>(kPerBand * 3));
    for (int i = 0; i < kPerBand; ++i) {
        EXPECT_EQ(order[i], "high-" + std::to_string(i));
        EXPECT_EQ(order[kPerBand + i], "mid-" + std::to_string(i));
        EXPECT_EQ(order[2 * kPerBand + i], "low-" + std::to_string(i));
    }
}

// --- Backoff-gated eligibility (Phase C) ----------------------------------------------
// Still not a single thread: the scheduler takes the current time as an argument rather
// than reading a clock, so the whole backoff schedule is a deterministic function call.

TEST_F(SchedulerCoreTest, AJobRequeuedWithABackoffIsNotEligibleUntilItsTimeArrives) {
    const SchedulerCore::TimePoint t0 = SchedulerCore::TimePoint{} + std::chrono::hours(1);
    SubmitJob("a");
    ASSERT_EQ(scheduler_.TakeNextEligible(t0), "a");
    scheduler_.Requeue("a", 0, t0 + std::chrono::seconds(5));

    EXPECT_FALSE(scheduler_.HasEligible(t0));
    EXPECT_FALSE(scheduler_.HasEligible(t0 + std::chrono::seconds(4)));
    EXPECT_EQ(scheduler_.TakeNextEligible(t0 + std::chrono::seconds(4)), std::nullopt);
    // It is still PENDING throughout -- held back, not forgotten.
    EXPECT_TRUE(scheduler_.IsPending("a"));

    EXPECT_TRUE(scheduler_.HasEligible(t0 + std::chrono::seconds(5)));
    EXPECT_EQ(scheduler_.TakeNextEligible(t0 + std::chrono::seconds(5)), "a");
}

TEST_F(SchedulerCoreTest, ABackoffDoesNotBlockOtherJobsBehindIt) {
    // The same "skipped, not blocking" rule dependencies already follow: a high-priority
    // job waiting out a backoff must not stall a lower-priority one that is ready.
    const SchedulerCore::TimePoint t0 = SchedulerCore::TimePoint{} + std::chrono::hours(1);
    SubmitJob("high", 10);
    SubmitJob("low", 0);
    ASSERT_EQ(scheduler_.TakeNextEligible(t0), "high");
    scheduler_.Requeue("high", 10, t0 + std::chrono::seconds(30));

    EXPECT_EQ(scheduler_.TakeNextEligible(t0), "low");
}

TEST_F(SchedulerCoreTest, NextEligibleTimeIsTheEarliestBackoffAWorkerCouldSleepUntil) {
    const SchedulerCore::TimePoint t0 = SchedulerCore::TimePoint{} + std::chrono::hours(1);
    SubmitJob("a");
    SubmitJob("b");
    ASSERT_TRUE(scheduler_.TakeNextEligible(t0).has_value());
    ASSERT_TRUE(scheduler_.TakeNextEligible(t0).has_value());
    scheduler_.Requeue("a", 0, t0 + std::chrono::seconds(30));
    scheduler_.Requeue("b", 0, t0 + std::chrono::seconds(5));

    // The nearer of the two: a worker that slept until the later one would leave "b"
    // sitting past its own deadline.
    EXPECT_EQ(scheduler_.NextEligibleTime(t0), t0 + std::chrono::seconds(5));
    // Once "b" is eligible, only "a" is still on a clock.
    EXPECT_EQ(scheduler_.NextEligibleTime(t0 + std::chrono::seconds(5)),
               t0 + std::chrono::seconds(30));
    // And with nothing waiting on a clock at all there is no deadline to sleep until --
    // a worker should wait on the notify instead, indefinitely.
    EXPECT_EQ(scheduler_.NextEligibleTime(t0 + std::chrono::seconds(30)), std::nullopt);
}

TEST_F(SchedulerCoreTest, AJobWaitingOnADependencyIsNotADeadlineToSleepUntil) {
    // It becomes eligible when the dependency finishes, which is a notify, not a clock.
    // Reporting it as a deadline would make workers spin.
    SubmitJob("first");
    SubmitJob("second", 0, {"first"});
    ASSERT_EQ(scheduler_.TakeNextEligible(), "first");
    EXPECT_EQ(scheduler_.NextEligibleTime(SchedulerCore::TimePoint{}), std::nullopt);
}

TEST_F(SchedulerCoreTest, TakingAJobConsumesItsBackoffSoAPlainRequeueRunsImmediately) {
    const SchedulerCore::TimePoint t0 = SchedulerCore::TimePoint{} + std::chrono::hours(1);
    SubmitJob("a");
    ASSERT_TRUE(scheduler_.TakeNextEligible(t0).has_value());
    scheduler_.Requeue("a", 0, t0 + std::chrono::seconds(5));
    ASSERT_EQ(scheduler_.TakeNextEligible(t0 + std::chrono::seconds(5)), "a");

    // A manual retry passes no backoff at all; the previous one must not linger.
    scheduler_.Requeue("a", 0);
    EXPECT_TRUE(scheduler_.HasEligible(t0));
}

}  // namespace
}  // namespace mediatool::jobs
