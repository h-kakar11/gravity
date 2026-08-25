#include "core/jobs/JobManager.h"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <thread>

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
