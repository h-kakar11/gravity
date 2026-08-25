#include "core/jobs/JobManager.h"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <thread>

#include "core/jobs/JobTypes.h"
#include "core/jobs/TestJob.h"

using mediatool::jobs::JobManager;
using mediatool::jobs::JobState;
using mediatool::jobs::TestJob;

namespace {

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
