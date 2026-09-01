#include "core/jobs/JobManager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "core/errors/ErrorInfo.h"
#include "core/errors/MediaToolException.h"
#include "core/jobs/JobTypes.h"
#include "core/jobs/TestJob.h"

using mediatool::errors::ErrorCategory;
using mediatool::errors::ErrorInfo;
using mediatool::errors::MediaToolException;
using mediatool::jobs::JobManager;
using mediatool::jobs::JobState;
using mediatool::jobs::JobType;
using mediatool::jobs::TestJob;

namespace {

// A Job whose Execute() returns (almost) instantly, so a submit/cancel stress test can
// run thousands of cycles per second -- unlike TestJob, whose ~1000ms run would make
// that impractically slow.
class InstantJob final : public mediatool::jobs::Job {
public:
    InstantJob() : Job(JobType::Test) {}

    void Execute() override {
        if (IsCancellationRequested()) {
            throw MediaToolException(
                ErrorInfo::Make("E_TEST_CANCELLED", ErrorCategory::Cancelled, "cancelled"));
        }
    }
};

// Polls GetJob(id).state until `predicate` is true or the deadline passes. Returns the
// last observed state so assertions can report what it actually settled on.
JobState WaitForState(JobManager& manager, const mediatool::jobs::JobId& id,
                      std::chrono::milliseconds timeout,
                      const std::function<bool(JobState)>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    JobState last = manager.GetJob(id).state;
    while (std::chrono::steady_clock::now() < deadline) {
        last = manager.GetJob(id).state;
        if (predicate(last)) return last;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return last;
}

bool IsTerminal(JobState state) {
    return state == JobState::Completed || state == JobState::Failed ||
           state == JobState::Cancelled;
}

}  // namespace

TEST(JobManager, RunsTestJobToCompletion) {
    JobManager manager(1);
    const auto id = manager.SubmitJob(std::make_unique<TestJob>());

    const JobState final = WaitForState(manager, id, std::chrono::seconds(5), IsTerminal);

    EXPECT_EQ(final, JobState::Completed);
    const auto snapshot = manager.GetJob(id);
    ASSERT_TRUE(snapshot.result.has_value());
    EXPECT_TRUE(snapshot.startedAt.has_value());
    EXPECT_TRUE(snapshot.completedAt.has_value());
}

TEST(JobManager, CancelStopsARunningJobPromptly) {
    JobManager manager(1);
    const auto id = manager.SubmitJob(std::make_unique<TestJob>());

    // TestJob's first checkpoint isn't reached for ~100ms; give it a moment to actually
    // start running before we cancel it.
    ASSERT_EQ(WaitForState(manager, id, std::chrono::seconds(2),
                           [](JobState s) { return s == JobState::Running; }),
              JobState::Running);

    const auto cancelRequestedAt = std::chrono::steady_clock::now();
    manager.CancelJob(id);

    const JobState final = WaitForState(manager, id, std::chrono::seconds(2), IsTerminal);
    const auto elapsed = std::chrono::steady_clock::now() - cancelRequestedAt;

    EXPECT_EQ(final, JobState::Cancelled);
    // TestJob's full run is ~1000ms (10 steps x 100ms); a prompt cancellation must land
    // well short of that, not run to completion before noticing the flag.
    EXPECT_LT(elapsed, std::chrono::milliseconds(600));
}

// Regression test for #4: a Queued job racing with a concurrent RequestCancel() must
// never crash the worker thread (previously: an uncaught E_INVALID_JOB_TRANSITION
// escaping RunJob -> std::terminate -> process abort, reproduced by the audit at
// ~1200-6800 submit/cancel cycles of unsynchronized timing). The race window itself is
// only a couple of instructions wide, so hitting it via raw timing is unreliable on any
// given machine/scheduler -- this test uses JobManager's testing-only pre-MarkStarting
// hook to force the exact interleaving deterministically on every iteration instead.
// Success means the process is still alive and every job reached a terminal state -- if
// the bug regressed, this whole test binary would abort partway through and CTest would
// report it as crashed, not merely failed.
TEST(JobManager, SubmitCancelRaceNeverCrashesTheWorker) {
    constexpr int kIterations = 500;
    JobManager manager(1);

    manager.SetPreMarkStartingHookForTesting(
        [&manager](const mediatool::jobs::JobId& id) { manager.CancelJob(id); });

    for (int i = 0; i < kIterations; ++i) {
        const auto id = manager.SubmitJob(std::make_unique<InstantJob>());
        const JobState final = WaitForState(manager, id, std::chrono::seconds(2), IsTerminal);
        ASSERT_TRUE(IsTerminal(final)) << "job " << id << " never reached a terminal state";
    }
}

// Regression test for #6: destroying/shutting down a JobManager must not start freshly
// -queued jobs and wait for them to run -- it should only ever wait on work already in
// flight (the one Running job here), cancelling everything still queued.
TEST(JobManager, ShutdownCancelsQueuedJobsAndDoesNotWaitForThem) {
    JobManager manager(1);
    const auto runningId = manager.SubmitJob(std::make_unique<TestJob>());
    ASSERT_EQ(WaitForState(manager, runningId, std::chrono::seconds(2),
                           [](JobState s) { return s == JobState::Running; }),
              JobState::Running);

    std::vector<mediatool::jobs::JobId> queuedIds;
    for (int i = 0; i < 5; ++i) queuedIds.push_back(manager.SubmitJob(std::make_unique<TestJob>()));
    for (const auto& id : queuedIds) EXPECT_EQ(manager.GetJob(id).state, JobState::Queued);

    const auto shutdownStart = std::chrono::steady_clock::now();
    manager.Shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - shutdownStart;

    // TestJob's full run is ~1000ms; if shutdown incorrectly ran all 5 queued jobs to
    // completion one after another (the old behavior), this would take several seconds.
    // Bounding it well under that proves shutdown only waited on the one already-Running
    // job's Cancel-triggered exit, not on freshly-started queued work.
    EXPECT_LT(elapsed, std::chrono::milliseconds(800));

    for (const auto& id : queuedIds) {
        EXPECT_EQ(manager.GetJob(id).state, JobState::Cancelled);
    }
    EXPECT_EQ(manager.GetJob(runningId).state, JobState::Cancelled);
}

// The pool JobManager reports must be the pool it actually started. A JobManager whose
// std::thread creation partially fails used to destroy a vector of joinable threads while
// unwinding out of its own constructor, which is an unconditional std::terminate -- the
// same "one setting value takes the process down" shape as #5. It now keeps whatever
// workers it managed to start and reports that number, so concurrency accounting
// downstream describes reality rather than an aspiration.
TEST(JobManager, ReportsTheWorkerPoolItActuallyStarted) {
    JobManager zeroRequested(0);
    EXPECT_EQ(zeroRequested.MaxConcurrentJobs(), 1u) << "0 workers is meaningless; 1 is the floor";

    constexpr std::size_t kRequested = 8;
    JobManager manager(kRequested);
    EXPECT_GE(manager.MaxConcurrentJobs(), 1u);
    EXPECT_LE(manager.MaxConcurrentJobs(), kRequested);

    // Whatever size it settled on, it must still run work.
    const auto id = manager.SubmitJob(std::make_unique<InstantJob>());
    EXPECT_TRUE(IsTerminal(WaitForState(manager, id, std::chrono::seconds(2), IsTerminal)));
}

TEST(JobManager, SecondJobWaitsForFirstWhenMaxConcurrentJobsIsOne) {
    JobManager manager(1);
    const auto id1 = manager.SubmitJob(std::make_unique<TestJob>());
    const auto id2 = manager.SubmitJob(std::make_unique<TestJob>());

    ASSERT_EQ(WaitForState(manager, id1, std::chrono::seconds(2),
                           [](JobState s) { return s == JobState::Running; }),
              JobState::Running);

    // With only one worker slot, the second job must still be waiting in the queue.
    EXPECT_EQ(manager.GetJob(id2).state, JobState::Queued);

    const JobState firstFinal =
        WaitForState(manager, id1, std::chrono::seconds(5), IsTerminal);
    EXPECT_EQ(firstFinal, JobState::Completed);

    const JobState secondFinal =
        WaitForState(manager, id2, std::chrono::seconds(5), IsTerminal);
    EXPECT_EQ(secondFinal, JobState::Completed);
}

// Verification for #4.8 (Parallel Processing): the complement of the concurrency=1 test
// above -- proves `concurrentJobs > 1` actually runs jobs in parallel rather than just
// accepting the setting and silently still serializing them. TestJob's full run is ~1000ms
// (10 steps x 100ms); if this were secretly serialized, only the first job would reach
// Running within the 500ms window below and the rest would still be Queued, each waiting
// on the one ahead of it.
TEST(JobManager, AllJobsRunConcurrentlyWhenMaxConcurrentJobsAllowsIt) {
    constexpr std::size_t kConcurrency = 4;
    JobManager manager(kConcurrency);

    std::vector<mediatool::jobs::JobId> ids;
    for (std::size_t i = 0; i < kConcurrency; ++i) {
        ids.push_back(manager.SubmitJob(std::make_unique<TestJob>()));
    }

    for (const auto& id : ids) {
        EXPECT_EQ(WaitForState(manager, id, std::chrono::milliseconds(500),
                               [](JobState s) { return s == JobState::Running; }),
                  JobState::Running)
            << "job " << id << " never reached Running promptly -- looks serialized, not parallel";
    }

    for (const auto& id : ids) {
        EXPECT_EQ(WaitForState(manager, id, std::chrono::seconds(5), IsTerminal),
                  JobState::Completed);
    }
}

// --- dependencies, end to end through the worker pool ------------------------------------
// SchedulerCoreTest covers the policy exhaustively without threads; these prove JobManager
// actually drives that policy -- that a dependent is not started early, that a failed
// dependency does not leave a job queued forever, and that the workers wake up when a
// dependency finishes rather than sleeping through it.

namespace {

// Fails on demand, so a dependency chain can be broken deliberately. Optionally waits for
// a release flag first: a chain has to be fully submitted before its root fails, because a
// dependency that has *already* failed is refused at submission (SchedulerCore::Submit)
// rather than accepted and cancelled -- so a test that wants to observe propagation must
// hold the failure until the dependents exist.
class FailingJob final : public mediatool::jobs::Job {
public:
    explicit FailingJob(std::atomic<bool>* releaseFlag = nullptr)
        : Job(JobType::Test), releaseFlag_(releaseFlag) {}

    void Execute() override {
        if (releaseFlag_) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!releaseFlag_->load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        throw MediaToolException(ErrorInfo::Make("E_TEST_FAILED", ErrorCategory::Unknown, "failed"));
    }

private:
    std::atomic<bool>* releaseFlag_;
};

std::unique_ptr<mediatool::jobs::Job> DependentInstantJob(std::vector<mediatool::jobs::JobId> dependsOn,
                                                           int priority = 0) {
    auto job = std::make_unique<InstantJob>();
    job->SetPriority(priority);
    job->SetDependsOn(std::move(dependsOn));
    return job;
}

}  // namespace

TEST(JobManager, ADependentJobDoesNotStartUntilItsDependencyCompletes) {
    JobManager manager(4);  // room to run it early if the dependency were ignored

    const auto first = manager.SubmitJob(std::make_unique<TestJob>());  // ~1000ms
    const auto second = manager.SubmitJob(DependentInstantJob({first}));

    ASSERT_EQ(WaitForState(manager, first, std::chrono::seconds(2),
                           [](JobState s) { return s == JobState::Running; }),
              JobState::Running);
    // Three idle workers and an instant job: if dependencies were not enforced this would
    // already be finished.
    EXPECT_EQ(manager.GetJob(second).state, JobState::Queued);

    EXPECT_EQ(WaitForState(manager, first, std::chrono::seconds(5), IsTerminal), JobState::Completed);
    EXPECT_EQ(WaitForState(manager, second, std::chrono::seconds(5), IsTerminal), JobState::Completed);

    // Ordering, not just completion: the dependent started after the dependency finished.
    const auto firstSnapshot = manager.GetJob(first);
    const auto secondSnapshot = manager.GetJob(second);
    ASSERT_TRUE(firstSnapshot.completedAt.has_value());
    ASSERT_TRUE(secondSnapshot.startedAt.has_value());
    EXPECT_GE(*secondSnapshot.startedAt, *firstSnapshot.completedAt);
}

TEST(JobManager, AFailedDependencyCancelsWhatWasWaitingOnIt) {
    JobManager manager(1);
    std::atomic<bool> release{false};
    const auto failing = manager.SubmitJob(std::make_unique<FailingJob>(&release));
    const auto dependent = manager.SubmitJob(DependentInstantJob({failing}));
    release = true;

    EXPECT_EQ(WaitForState(manager, failing, std::chrono::seconds(5), IsTerminal), JobState::Failed);
    // Without propagation this job would sit QUEUED forever, blocking nothing and
    // finishing never -- the queue's version of a leak.
    EXPECT_EQ(WaitForState(manager, dependent, std::chrono::seconds(2), IsTerminal),
              JobState::Cancelled);
}

TEST(JobManager, DependencyFailurePropagatesDownAWholeChain) {
    JobManager manager(2);
    std::atomic<bool> release{false};
    const auto a = manager.SubmitJob(std::make_unique<FailingJob>(&release));
    const auto b = manager.SubmitJob(DependentInstantJob({a}));
    const auto c = manager.SubmitJob(DependentInstantJob({b}));
    release = true;  // the whole chain exists now; let the root fail

    EXPECT_EQ(WaitForState(manager, a, std::chrono::seconds(5), IsTerminal), JobState::Failed);
    EXPECT_EQ(WaitForState(manager, b, std::chrono::seconds(2), IsTerminal), JobState::Cancelled);
    EXPECT_EQ(WaitForState(manager, c, std::chrono::seconds(2), IsTerminal), JobState::Cancelled);
}

TEST(JobManager, AnImpossibleDependencyIsRejectedAtSubmission) {
    JobManager manager(1);

    // An id no job has ever had: accepting this would queue a job that can never run.
    EXPECT_THROW(manager.SubmitJob(DependentInstantJob({"job-does-not-exist"})), MediaToolException);

    const auto completed = manager.SubmitJob(std::make_unique<InstantJob>());
    ASSERT_EQ(WaitForState(manager, completed, std::chrono::seconds(2), IsTerminal),
              JobState::Completed);
    // Depending on a job that already completed is fine and starts immediately.
    const auto after = manager.SubmitJob(DependentInstantJob({completed}));
    EXPECT_EQ(WaitForState(manager, after, std::chrono::seconds(2), IsTerminal), JobState::Completed);
}

TEST(JobManager, ManyWaitingJobsDoNotKeepTheWorkersSpinning) {
    // A queue full of jobs that cannot run must leave the pool asleep on its condition
    // variable, not spinning through ineligible entries. Asserted indirectly but honestly:
    // 500 blocked jobs, then the dependency completes and every one of them runs.
    JobManager manager(4);
    const auto blocker = manager.SubmitJob(std::make_unique<TestJob>());  // ~1000ms

    std::vector<mediatool::jobs::JobId> waiting;
    waiting.reserve(500);
    for (int i = 0; i < 500; ++i) waiting.push_back(manager.SubmitJob(DependentInstantJob({blocker})));

    ASSERT_EQ(WaitForState(manager, blocker, std::chrono::seconds(5), IsTerminal),
              JobState::Completed);
    for (const auto& id : waiting) {
        ASSERT_EQ(WaitForState(manager, id, std::chrono::seconds(10), IsTerminal), JobState::Completed)
            << "job " << id << " never ran after its dependency completed";
    }
}

TEST(JobManager, HigherPriorityQueuedWorkRunsFirst) {
    // The pool is occupied by one long job, so everything else queues up behind it and the
    // order they come out in is entirely the scheduler's decision.
    JobManager manager(1);
    const auto occupier = manager.SubmitJob(std::make_unique<TestJob>());
    ASSERT_EQ(WaitForState(manager, occupier, std::chrono::seconds(2),
                           [](JobState s) { return s == JobState::Running; }),
              JobState::Running);

    std::vector<mediatool::jobs::JobId> ids;
    for (int priority : {0, 50, 0, 100}) {
        auto job = std::make_unique<InstantJob>();
        job->SetPriority(priority);
        ids.push_back(manager.SubmitJob(std::move(job)));
    }
    manager.CancelJob(occupier);

    for (const auto& id : ids) {
        ASSERT_TRUE(IsTerminal(WaitForState(manager, id, std::chrono::seconds(5), IsTerminal)));
    }

    // startedAt is an ISO-8601 UTC string, so lexicographic order is chronological order.
    const auto startedAt = [&manager](const mediatool::jobs::JobId& id) {
        const auto snapshot = manager.GetJob(id);
        return snapshot.startedAt.value_or("");
    };
    EXPECT_LE(startedAt(ids[3]), startedAt(ids[1])) << "priority 100 must precede priority 50";
    EXPECT_LE(startedAt(ids[1]), startedAt(ids[0])) << "priority 50 must precede priority 0";
    EXPECT_LE(startedAt(ids[0]), startedAt(ids[2])) << "equal priorities keep FIFO order";
}

TEST(JobManager, AJobStillDependedOnCannotBeRemoved) {
    JobManager manager(1);
    const auto blocker = manager.SubmitJob(std::make_unique<TestJob>());
    const auto dependent = manager.SubmitJob(DependentInstantJob({blocker}));

    ASSERT_EQ(WaitForState(manager, blocker, std::chrono::seconds(5), IsTerminal),
              JobState::Completed);
    // The dependency is terminal, so removeJob would normally be allowed -- but its
    // dependent may not have run yet, and dropping the record it is waiting on would
    // strand it.
    if (manager.GetJob(dependent).state == JobState::Queued) {
        EXPECT_THROW(manager.RemoveJob(blocker), MediaToolException);
    }

    ASSERT_EQ(WaitForState(manager, dependent, std::chrono::seconds(5), IsTerminal),
              JobState::Completed);
    EXPECT_NO_THROW(manager.RemoveJob(blocker));
    EXPECT_NO_THROW(manager.RemoveJob(dependent));
}

// --- Automatic retry (Phase C) --------------------------------------------------------

namespace {

// Fails with a scripted error for its first `failuresBeforeSuccess` attempts, then
// succeeds. Records the exact instants each attempt started so a test can assert the
// backoff actually elapsed rather than assuming it did.
class FlakyJob final : public mediatool::jobs::Job {
public:
    FlakyJob(int failuresBeforeSuccess, ErrorInfo error)
        : Job(JobType::Test), failuresBeforeSuccess_(failuresBeforeSuccess),
          error_(std::move(error)) {}

    void Execute() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            startTimes_.push_back(std::chrono::steady_clock::now());
        }
        if (++started_ <= failuresBeforeSuccess_) {
            throw MediaToolException(error_);
        }
    }

    int TimesStarted() const { return started_; }
    std::vector<std::chrono::steady_clock::time_point> StartTimes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return startTimes_;
    }

private:
    std::atomic<int> started_{0};
    int failuresBeforeSuccess_;
    ErrorInfo error_;
    mutable std::mutex mutex_;
    std::vector<std::chrono::steady_clock::time_point> startTimes_;
};

mediatool::jobs::RetryPolicy FastRetryPolicy(int maxAttempts) {
    mediatool::jobs::RetryPolicy policy;
    policy.maxAttempts = maxAttempts;
    // Short enough to keep the test quick, long enough to be measurable -- the backoff
    // assertion below would be meaningless at zero.
    policy.initialBackoff = std::chrono::milliseconds(40);
    policy.backoffMultiplier = 2.0;
    policy.maxBackoff = std::chrono::milliseconds(200);
    return policy;
}

}  // namespace

TEST(JobManagerRetry, ARecoverableFailureIsRetriedUntilItSucceeds) {
    JobManager manager(1, FastRetryPolicy(3));
    auto job = std::make_unique<FlakyJob>(
        2, ErrorInfo::Make("E_NETWORK", ErrorCategory::NetworkError, "flaky", "", true));
    FlakyJob* raw = job.get();
    const auto id = manager.SubmitJob(std::move(job));

    const JobState final = WaitForState(manager, id, std::chrono::seconds(5),
                                         [](JobState s) { return s == JobState::Completed; });
    EXPECT_EQ(final, JobState::Completed);
    EXPECT_EQ(raw->TimesStarted(), 3);
    // The snapshot reports attempts, so a UI can say "attempt 3 of 3" rather than
    // silently re-running a job the user watched fail.
    EXPECT_EQ(manager.GetJob(id).attempts, 3);
}

TEST(JobManagerRetry, EachRetryWaitsOutAnIncreasingBackoff) {
    JobManager manager(1, FastRetryPolicy(3));
    auto job = std::make_unique<FlakyJob>(
        2, ErrorInfo::Make("E_NETWORK", ErrorCategory::NetworkError, "flaky", "", true));
    FlakyJob* raw = job.get();
    const auto id = manager.SubmitJob(std::move(job));

    ASSERT_EQ(WaitForState(manager, id, std::chrono::seconds(5),
                            [](JobState s) { return s == JobState::Completed; }),
               JobState::Completed);

    const auto starts = raw->StartTimes();
    ASSERT_EQ(starts.size(), 3u);
    const auto firstGap =
        std::chrono::duration_cast<std::chrono::milliseconds>(starts[1] - starts[0]);
    const auto secondGap =
        std::chrono::duration_cast<std::chrono::milliseconds>(starts[2] - starts[1]);
    // Lower bounds only. Asserting an upper bound would make this a timing test that a
    // loaded CI box fails for no good reason; what matters is that the wait happened and
    // that it grew.
    EXPECT_GE(firstGap, std::chrono::milliseconds(35));
    EXPECT_GE(secondGap, std::chrono::milliseconds(75));
}

TEST(JobManagerRetry, RetryingIsNotTerminalSoDependentsAreNotCancelled) {
    // The reason RUNNING -> RETRYING exists as its own transition. Routing an automatic
    // retry through FAILED tells the scheduler the job ended, which cancels every job
    // waiting on it -- an entire dependent chain torn down by one transient blip that was
    // about to be retried anyway.
    JobManager manager(1, FastRetryPolicy(3));
    auto flaky = std::make_unique<FlakyJob>(
        1, ErrorInfo::Make("E_NETWORK", ErrorCategory::NetworkError, "flaky", "", true));
    const auto firstId = manager.SubmitJob(std::move(flaky));

    auto dependent = std::make_unique<InstantJob>();
    dependent->SetDependsOn({firstId});
    const auto dependentId = manager.SubmitJob(std::move(dependent));

    EXPECT_EQ(WaitForState(manager, firstId, std::chrono::seconds(5),
                            [](JobState s) { return s == JobState::Completed; }),
               JobState::Completed);
    EXPECT_EQ(WaitForState(manager, dependentId, std::chrono::seconds(5),
                            [](JobState s) { return s == JobState::Completed; }),
               JobState::Completed);
}

TEST(JobManagerRetry, APermanentFailureIsNotRetriedAtAll) {
    JobManager manager(1, FastRetryPolicy(3));
    auto job = std::make_unique<FlakyJob>(
        99, ErrorInfo::Make("E_DISK_FULL", ErrorCategory::DiskSpaceError, "full", "", true));
    FlakyJob* raw = job.get();
    const auto id = manager.SubmitJob(std::move(job));

    EXPECT_EQ(WaitForState(manager, id, std::chrono::seconds(5),
                            [](JobState s) { return s == JobState::Failed; }),
               JobState::Failed);
    // Exactly once. A recoverable flag on a disk-full error must not turn one clear
    // failure into three identical ones several seconds apart.
    EXPECT_EQ(raw->TimesStarted(), 1);
}

TEST(JobManagerRetry, RetriesStopAtTheLimitAndTheJobEndsFailedWithItsLastError) {
    JobManager manager(1, FastRetryPolicy(2));
    auto job = std::make_unique<FlakyJob>(
        99, ErrorInfo::Make("E_NETWORK", ErrorCategory::NetworkError, "always flaky", "", true));
    FlakyJob* raw = job.get();
    const auto id = manager.SubmitJob(std::move(job));

    EXPECT_EQ(WaitForState(manager, id, std::chrono::seconds(5),
                            [](JobState s) { return s == JobState::Failed; }),
               JobState::Failed);
    EXPECT_EQ(raw->TimesStarted(), 2);  // the original plus one retry
    const auto snapshot = manager.GetJob(id);
    ASSERT_TRUE(snapshot.error.has_value());
    EXPECT_EQ(snapshot.error->code, "E_NETWORK");
    EXPECT_EQ(snapshot.attempts, 2);
}

TEST(JobManagerRetry, ASeededAttemptCountIsNotAFreshBudget) {
    // What makes the retry budget survive a crash: a job rebuilt with two attempts already
    // spent gets the third and stops, rather than three more.
    JobManager manager(1, FastRetryPolicy(3));
    auto job = std::make_unique<FlakyJob>(
        99, ErrorInfo::Make("E_NETWORK", ErrorCategory::NetworkError, "flaky", "", true));
    job->SetAttemptCount(2);
    FlakyJob* raw = job.get();
    const auto id = manager.SubmitJob(std::move(job));

    EXPECT_EQ(WaitForState(manager, id, std::chrono::seconds(5),
                            [](JobState s) { return s == JobState::Failed; }),
               JobState::Failed);
    EXPECT_EQ(raw->TimesStarted(), 1);
    EXPECT_EQ(manager.GetJob(id).attempts, 3);
}

TEST(JobManagerRetry, AJobWaitingOutABackoffCanStillBeCancelled) {
    // A backoff is exactly when a user gives up. Waiting out the timer first would make
    // Cancel feel broken.
    mediatool::jobs::RetryPolicy policy = FastRetryPolicy(3);
    policy.initialBackoff = std::chrono::seconds(30);
    policy.maxBackoff = std::chrono::seconds(30);
    JobManager manager(1, policy);

    auto job = std::make_unique<FlakyJob>(
        99, ErrorInfo::Make("E_NETWORK", ErrorCategory::NetworkError, "flaky", "", true));
    const auto id = manager.SubmitJob(std::move(job));

    ASSERT_EQ(WaitForState(manager, id, std::chrono::seconds(5),
                            [](JobState s) { return s == JobState::Retrying; }),
               JobState::Retrying);
    manager.CancelJob(id);
    EXPECT_EQ(WaitForState(manager, id, std::chrono::seconds(5),
                            [](JobState s) { return s == JobState::Cancelled; }),
               JobState::Cancelled);
}

