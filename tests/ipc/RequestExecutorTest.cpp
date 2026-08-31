#include "core/ipc/RequestExecutor.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace mediatool::ipc {
namespace {

using namespace std::chrono_literals;

// A latch a test can hold tasks on, so "is this actually running off the caller's thread"
// and "does the queue bound hold" can be asserted without sleeping and hoping.
class Latch {
public:
    void Wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return released_; });
    }
    void Release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool released_ = false;
};

// Spin-waits for `predicate` rather than sleeping a fixed amount: these tests assert on
// what the executor did, not on how fast the machine running them is.
template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

TEST(RequestExecutor, RunsSubmittedWorkOnItsOwnThreads) {
    RequestExecutor executor(2, 8);

    std::atomic<int> completed{0};
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(executor.TrySubmit([&completed] { ++completed; }));
    }

    EXPECT_TRUE(WaitFor([&completed] { return completed.load() == 4; }));
}

TEST(RequestExecutor, SubmissionDoesNotBlockTheCaller) {
    // The whole reason this class exists: handing off a slow request must return
    // immediately so the IPC loop can read the next line.
    RequestExecutor executor(1, 8);
    Latch latch;

    const auto before = std::chrono::steady_clock::now();
    ASSERT_TRUE(executor.TrySubmit([&latch] { latch.Wait(); }));
    const auto elapsed = std::chrono::steady_clock::now() - before;

    EXPECT_LT(elapsed, 500ms) << "TrySubmit must not wait for the task to run";
    latch.Release();
}

TEST(RequestExecutor, RejectsWorkOnceTheQueueBoundIsReached) {
    // Backpressure is a feature: an unbounded queue turns a client that never stops asking
    // into unbounded memory growth. The caller is told "no" and answers the request itself.
    constexpr std::size_t kMaxQueued = 3;
    RequestExecutor executor(1, kMaxQueued);
    Latch blocker;

    ASSERT_TRUE(executor.TrySubmit([&blocker] { blocker.Wait(); }));  // occupies the only thread

    // Fill the queue behind it. (The occupying task may or may not have been dequeued yet,
    // so accept up to one extra acceptance before saturation.)
    std::size_t accepted = 0;
    for (std::size_t i = 0; i < kMaxQueued + 2; ++i) {
        if (executor.TrySubmit([] {})) ++accepted;
    }
    EXPECT_LE(accepted, kMaxQueued + 1);
    EXPECT_FALSE(executor.TrySubmit([] {})) << "a saturated executor must refuse, not grow";

    blocker.Release();
}

TEST(RequestExecutor, AThrowingTaskDoesNotKillItsWorker) {
    // A task is a whole request including its response write. If one throwing task retired
    // a thread, the pool would silently shrink to nothing and later requests would never
    // be answered.
    RequestExecutor executor(1, 8);

    ASSERT_TRUE(executor.TrySubmit([] { throw std::runtime_error("boom"); }));
    ASSERT_TRUE(executor.TrySubmit([] { throw 42; }));

    std::atomic<bool> ranAfterwards{false};
    ASSERT_TRUE(executor.TrySubmit([&ranAfterwards] { ranAfterwards = true; }));

    EXPECT_TRUE(WaitFor([&ranAfterwards] { return ranAfterwards.load(); }))
        << "the worker that swallowed two thrown tasks must still be serving";
    EXPECT_EQ(executor.ThreadCount(), 1u);
}

TEST(RequestExecutor, ShutdownWaitsForWorkAlreadyRunning) {
    RequestExecutor executor(2, 8);
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    ASSERT_TRUE(executor.TrySubmit([&started, &finished] {
        started = true;
        std::this_thread::sleep_for(50ms);
        finished = true;
    }));
    ASSERT_TRUE(WaitFor([&started] { return started.load(); }));

    executor.Shutdown();
    EXPECT_TRUE(finished.load()) << "in-flight work may be holding a subprocess; it is waited for";

    EXPECT_NO_THROW(executor.Shutdown());
    EXPECT_FALSE(executor.TrySubmit([] {})) << "a shut-down executor accepts nothing";
}

TEST(RequestExecutor, ShutdownDiscardsWorkThatHasNotStarted) {
    // Deliberate, and documented on Shutdown(): it runs when stdin has closed, so the
    // client that sent those queued requests is gone and no response would reach anyone.
    // Draining them instead would delay exit for no one's benefit.
    RequestExecutor executor(1, 8);
    Latch blocker;
    std::atomic<bool> queuedTaskRan{false};

    ASSERT_TRUE(executor.TrySubmit([&blocker] { blocker.Wait(); }));
    ASSERT_TRUE(executor.TrySubmit([&queuedTaskRan] { queuedTaskRan = true; }));
    ASSERT_TRUE(WaitFor([&executor] { return executor.PendingCount() == 1; }));

    // Shutdown() has to run while the only worker is still occupied, so the second task is
    // provably still queued when it happens -- releasing the blocker first would let the
    // worker pick that task up and turn this into a coin flip.
    std::thread shutdownThread([&executor] { executor.Shutdown(); });
    ASSERT_TRUE(WaitFor([&executor] { return executor.PendingCount() == 0; }))
        << "Shutdown() should have cleared the queue before joining";
    blocker.Release();
    shutdownThread.join();

    EXPECT_FALSE(queuedTaskRan.load());
}

TEST(RequestExecutor, ManyConcurrentSubmissionsAreAllRunExactlyOnce) {
    RequestExecutor executor(4, 512);
    std::atomic<int> counter{0};
    std::atomic<int> submitted{0};

    std::vector<std::thread> submitters;
    for (int t = 0; t < 4; ++t) {
        submitters.emplace_back([&executor, &counter, &submitted] {
            for (int i = 0; i < 50; ++i) {
                if (executor.TrySubmit([&counter] { ++counter; })) ++submitted;
            }
        });
    }
    for (auto& submitter : submitters) submitter.join();

    // Waited for rather than shut down first: Shutdown() drops work that has not started
    // (see the test above), so joining immediately would be asserting on a race of this
    // test's own making. What matters here is that every accepted task runs exactly once.
    EXPECT_TRUE(WaitFor([&counter, &submitted] { return counter.load() == submitted.load(); }))
        << "ran " << counter.load() << " of " << submitted.load() << " accepted tasks";
    executor.Shutdown();
    EXPECT_EQ(counter.load(), submitted.load()) << "no task may run twice";
}

}  // namespace
}  // namespace mediatool::ipc
