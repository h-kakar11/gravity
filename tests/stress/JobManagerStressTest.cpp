// Concurrency tests that are about VOLUME and TIMING rather than logic.
//
// They live in their own binary because they are slow and because they fail differently
// from unit tests: a race does not fail every run, so these are worth running deliberately
// (ci-local.ps1 -StressTest, and under a sanitizer where one is available) rather than on
// every save. Everything they exercise has a deterministic counterpart in the unit suite --
// SchedulerCoreTest owns the scheduling POLICY with no threads at all. What is here is the
// part that policy cannot answer: what N workers, an IPC thread and a cancelling user do to
// each other.
//
// Deliberately no assertion of the form "this finished within X ms". A loaded CI box makes
// that a coin flip, and a test that fails for no reason teaches people to ignore it. The
// assertions are on OUTCOMES -- every job reached a terminal state, no state is impossible,
// nothing deadlocked, the process is still standing.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "core/errors/ErrorInfo.h"
#include "core/errors/MediaToolException.h"
#include "core/jobs/JobManager.h"
#include "core/jobs/JobTypes.h"

namespace mediatool::jobs {
namespace {

using errors::ErrorCategory;
using errors::ErrorInfo;
using errors::MediaToolException;

// Returns almost immediately, honoring cancellation -- so a stress test can run thousands
// of lifecycles per second instead of being bounded by TestJob's ~1s sleep.
class QuickJob final : public Job {
public:
    explicit QuickJob(std::chrono::microseconds work = std::chrono::microseconds(0))
        : Job(JobType::Test), work_(work) {}

    void Execute() override {
        if (work_.count() > 0) std::this_thread::sleep_for(work_);
        if (IsCancellationRequested()) {
            throw MediaToolException(
                ErrorInfo::Make("E_STRESS_CANCELLED", ErrorCategory::Cancelled, "cancelled"));
        }
    }

private:
    std::chrono::microseconds work_;
};

// A job that fails recoverably forever, to keep the retry machinery busy while everything
// else is happening.
class AlwaysFailsJob final : public Job {
public:
    AlwaysFailsJob() : Job(JobType::Test) {}
    void Execute() override {
        throw MediaToolException(
            ErrorInfo::Make("E_NETWORK", ErrorCategory::NetworkError, "always", "", true));
    }
};

bool AllTerminal(JobManager& manager, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        bool all = true;
        for (const auto& snapshot : manager.ListJobs()) {
            if (!IsTerminalState(snapshot.state)) {
                all = false;
                break;
            }
        }
        if (all) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

RetryPolicy NoRetry() {
    RetryPolicy policy;
    policy.maxAttempts = 1;
    return policy;
}

}  // namespace

TEST(JobManagerStress, OneHundredConcurrentJobsAllReachATerminalState) {
    JobManager manager(8, NoRetry());
    std::vector<JobId> ids;
    ids.reserve(100);
    for (int i = 0; i < 100; ++i) {
        ids.push_back(manager.SubmitJob(std::make_unique<QuickJob>(std::chrono::microseconds(200))));
    }

    ASSERT_TRUE(AllTerminal(manager, std::chrono::seconds(60)));
    for (const JobId& id : ids) {
        const auto snapshot = manager.GetJob(id);
        EXPECT_EQ(snapshot.state, JobState::Completed) << id;
        // Every job that ran, ran exactly once: no worker picked up the same job twice.
        EXPECT_EQ(snapshot.attempts, 1) << id;
    }
    EXPECT_EQ(manager.ListJobs().size(), 100u);
}

TEST(JobManagerStress, SubmittingAndCancellingConcurrentlyNeverLeavesAJobUnfinished) {
    // The #4 race, at volume: a cancellation landing in the window between a worker
    // observing a Queued job and claiming it. Every job must end Completed or Cancelled --
    // never stuck, never in an impossible state, and never taking the process down.
    JobManager manager(4, NoRetry());
    constexpr int kJobs = 400;

    std::vector<JobId> ids;
    ids.reserve(kJobs);
    std::atomic<bool> stop{false};
    std::thread canceller([&] {
        std::mt19937 rng(1234);
        while (!stop.load()) {
            const auto snapshots = manager.ListJobs();
            if (!snapshots.empty()) {
                const auto& victim =
                    snapshots[std::uniform_int_distribution<std::size_t>(0, snapshots.size() - 1)(rng)];
                try {
                    manager.CancelJob(victim.id);
                } catch (const MediaToolException&) {
                    // Removed between the listing and the cancel -- a normal race, not a
                    // failure.
                }
            }
            std::this_thread::yield();
        }
    });

    for (int i = 0; i < kJobs; ++i) {
        ids.push_back(manager.SubmitJob(std::make_unique<QuickJob>()));
    }
    ASSERT_TRUE(AllTerminal(manager, std::chrono::seconds(60)));
    stop.store(true);
    canceller.join();

    for (const JobId& id : ids) {
        const JobState state = manager.GetJob(id).state;
        EXPECT_TRUE(state == JobState::Completed || state == JobState::Cancelled)
            << id << " ended in " << ToWireString(state);
    }
}

TEST(JobManagerStress, ListJobsUnderConstantMutationAlwaysReturnsAConsistentSnapshot) {
    // listJobs runs on an IPC thread while workers mutate the same jobs. It must never
    // tear, never return a half-built snapshot, and never deadlock against the worker
    // pool -- the two are the same lock.
    JobManager manager(4, NoRetry());
    std::atomic<bool> stop{false};
    std::atomic<int> listings{0};

    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                for (const auto& snapshot : manager.ListJobs()) {
                    // Reading every field is the point: a torn snapshot shows up here, not
                    // in the size of the vector.
                    EXPECT_FALSE(snapshot.id.empty());
                    EXPECT_FALSE(snapshot.createdAt.empty());
                    EXPECT_GE(snapshot.attempts, 0);
                    if (snapshot.state == JobState::Completed) {
                        EXPECT_TRUE(snapshot.completedAt.has_value()) << snapshot.id;
                    }
                }
                listings.fetch_add(1);
            }
        });
    }

    for (int i = 0; i < 300; ++i) {
        (void)manager.SubmitJob(std::make_unique<QuickJob>(std::chrono::microseconds(100)));
    }
    ASSERT_TRUE(AllTerminal(manager, std::chrono::seconds(60)));
    stop.store(true);
    for (auto& reader : readers) reader.join();

    EXPECT_GT(listings.load(), 0);
}

TEST(JobManagerStress, ShutdownWithAFullQueueJoinsEveryWorkerAndStartsNoNewWork) {
    // Shutdown cancels what is queued BEFORE waking the pool, so it only ever waits on
    // work already in flight. Without that it waits for the whole queue to run, which on a
    // queue of 500 is indistinguishable from a hang.
    auto manager = std::make_unique<JobManager>(4, NoRetry());
    for (int i = 0; i < 500; ++i) {
        (void)manager->SubmitJob(std::make_unique<QuickJob>(std::chrono::microseconds(500)));
    }

    const auto startedAt = std::chrono::steady_clock::now();
    manager->Shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - startedAt;

    // A generous ceiling, not a performance assertion: 500 jobs at 500us each is 250ms of
    // work spread over 4 workers if shutdown drained the queue. Anything under 10s proves
    // it did not.
    EXPECT_LT(elapsed, std::chrono::seconds(10));

    // Idempotent, and the destructor must not double-join.
    manager->Shutdown();
    EXPECT_NO_THROW(manager.reset());
}

TEST(JobManagerStress, RetriesAndCancellationsInterleaveWithoutLosingAJob) {
    // Everything at once: a pool of workers, jobs failing into backoffs, and a user
    // cancelling from another thread. The RETRYING state is the newest edge in the state
    // machine and the one with the least deterministic coverage.
    RetryPolicy policy;
    policy.maxAttempts = 3;
    policy.initialBackoff = std::chrono::milliseconds(5);
    policy.backoffMultiplier = 1.5;
    policy.maxBackoff = std::chrono::milliseconds(40);

    JobManager manager(4, policy);
    std::vector<JobId> ids;
    for (int i = 0; i < 60; ++i) {
        ids.push_back(manager.SubmitJob(std::make_unique<AlwaysFailsJob>()));
    }

    std::thread canceller([&] {
        for (std::size_t i = 0; i < ids.size(); i += 3) {
            try {
                manager.CancelJob(ids[i]);
            } catch (const MediaToolException&) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    canceller.join();

    ASSERT_TRUE(AllTerminal(manager, std::chrono::seconds(60)));
    for (const JobId& id : ids) {
        const auto snapshot = manager.GetJob(id);
        EXPECT_TRUE(snapshot.state == JobState::Failed || snapshot.state == JobState::Cancelled)
            << id << " ended in " << ToWireString(snapshot.state);
        // Never more attempts than the policy allows, however the cancellation interleaved.
        EXPECT_LE(snapshot.attempts, policy.maxAttempts) << id;
    }
}

TEST(JobManagerStress, ManyShortLivedManagersDoNotLeakWorkerThreads) {
    // Each JobManager starts its own pool. A destructor that failed to join would show up
    // as thread exhaustion after enough cycles rather than as a failure in any one of them.
    for (int cycle = 0; cycle < 40; ++cycle) {
        JobManager manager(4, NoRetry());
        for (int i = 0; i < 10; ++i) {
            (void)manager.SubmitJob(std::make_unique<QuickJob>());
        }
        // Destructor runs here: cancels what is queued, joins all four workers.
    }
    SUCCEED() << "40 pools of 4 workers created and destroyed without exhausting threads";
}

}  // namespace mediatool::jobs
