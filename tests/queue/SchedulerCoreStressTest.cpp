// Phase 8 concurrency/volume stress testing. SchedulerCore is pure and clock-free (see
// SchedulerCoreTest.cpp's header comment), which is exactly what makes a large,
// deterministic, seeded-random operation sequence possible with no sleeps, no threads, and
// a fully reproducible failure if one ever turns up: same seed, same sequence, every time.
//
// This does not re-test individual behaviors (SchedulerCoreTest.cpp already covers those
// in isolation) -- it drives thousands of insert/dispatch/transition/retry/move/priority/
// cancel/dependency/history operations in combination, modeled on how the real caller
// (JobManager) actually uses this API -- e.g. a job only ever becomes Running because
// SelectDispatchable said it could, never by injecting a raw state transition SchedulerCore
// was never told to admit. (An earlier version of this test injected raw transitions
// directly and "found" the concurrency limit and retry budget being violated -- which
// turned out to be this test breaking a contract ValidateInvariants() correctly enforces on
// its caller, not a product bug. Left as a lesson in the commit history, not a comment
// here.) The theory this test is actually for: a real invariant violation is more likely to
// show up from an unusual COMBINATION of otherwise-correct operations than from any one of
// them alone.

#include "core/queue/SchedulerCore.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using mediatool::jobs::JobState;
using mediatool::jobs::JobType;
using mediatool::queue::HistoryScope;
using mediatool::queue::JobPriority;
using mediatool::queue::JobRecord;
using mediatool::queue::MoveDirection;
using mediatool::queue::SchedulerCore;

namespace {

JobRecord MakeRecord(const std::string& id, JobPriority priority, JobType type) {
    JobRecord record;
    record.id = id;
    record.priority = priority;
    record.spec.type = type;
    return record;
}

void ExpectConsistent(const SchedulerCore& scheduler, const std::string& whenDescription) {
    const auto violations = scheduler.ValidateInvariants();
    ASSERT_TRUE(violations.empty()) << [&] {
        std::string joined = "after " + whenDescription + ":";
        for (const auto& v : violations) joined += "\n  - " + v;
        return joined;
    }();
}

// A real caller's view of one job's current attempt/maxRetries, looked up from a fresh
// Snapshot() rather than tracked independently -- so this test can never drift from what
// the scheduler itself actually believes.
struct RetryBudget {
    int attempt = 0;
    int maxRetries = 0;
};

RetryBudget LookUpRetryBudget(const SchedulerCore& scheduler, const std::string& id) {
    for (const auto& record : scheduler.Snapshot()) {
        if (record.id == id) return {record.attempt, record.retryPolicy.maxRetries};
    }
    return {};
}

TEST(SchedulerCoreStressTest, ThousandsOfRealisticOperationsNeverBreakInvariants) {
    SchedulerCore::Config config;
    config.maxConcurrency = 4;
    config.agingIntervalMs = 1000;
    config.historyLimit = 50;  // deliberately small, to actually exercise eviction under load
    SchedulerCore scheduler(config);

    std::mt19937 rng(0xC0FFEE);  // fixed seed: a failure here must reproduce identically
    std::uniform_int_distribution<int> pickOp(0, 8);
    std::uniform_int_distribution<int> pickPriority(0, 2);
    const JobPriority priorities[] = {JobPriority::Low, JobPriority::Normal, JobPriority::High};
    const MoveDirection directions[] = {MoveDirection::Up, MoveDirection::Down, MoveDirection::Top,
                                        MoveDirection::Bottom};

    std::vector<std::string> everInserted;
    // Jobs SelectDispatchable actually admitted (already Starting) and not yet terminal --
    // the only jobs this test is allowed to advance toward Running/Completed/Failed, the
    // same restriction JobManager itself observes.
    std::vector<std::string> dispatched;
    std::int64_t now = 0;
    constexpr int kOperations = 5000;

    auto removeFromDispatched = [&](const std::string& id) {
        dispatched.erase(std::remove(dispatched.begin(), dispatched.end(), id), dispatched.end());
    };

    for (int i = 0; i < kOperations; ++i) {
        now += 10;
        const int op = pickOp(rng);

        try {
            switch (op) {
                case 0: {  // insert a new job, occasionally depending on a prior one
                    const std::string id = "job-" + std::to_string(i);
                    JobRecord record =
                        MakeRecord(id, priorities[pickPriority(rng)],
                                  (i % 3 == 0) ? JobType::Conversion : JobType::Download);
                    if (!everInserted.empty() && i % 4 == 0) {
                        record.dependencies = {everInserted[rng() % everInserted.size()]};
                    }
                    scheduler.Insert(std::move(record), now);
                    everInserted.push_back(id);
                    break;
                }
                case 1: {  // admit whatever's eligible -- this is the ONLY way a job enters
                          // `dispatched`, exactly mirroring JobManager's dispatch loop.
                    for (auto& id : scheduler.SelectDispatchable(now)) dispatched.push_back(id);
                    break;
                }
                case 2:  // Starting -> Running, for an admitted job
                    if (!dispatched.empty()) {
                        scheduler.SetState(dispatched[rng() % dispatched.size()], JobState::Running, now);
                    }
                    break;
                case 3:  // Running -> Completed, for an admitted job; frees its slot
                    if (!dispatched.empty()) {
                        const std::size_t idx = rng() % dispatched.size();
                        scheduler.SetState(dispatched[idx], JobState::Completed, now);
                        removeFromDispatched(dispatched[idx]);
                        scheduler.ResolveDependencies(now);
                    }
                    break;
                case 4:  // Running -> Failed, for an admitted job; frees its slot
                    if (!dispatched.empty()) {
                        const std::size_t idx = rng() % dispatched.size();
                        scheduler.SetState(dispatched[idx], JobState::Failed, now);
                        removeFromDispatched(dispatched[idx]);
                        scheduler.ResolveDependencies(now);
                    }
                    break;
                case 5:  // a bounded automatic retry -- only for a job genuinely under budget,
                        // the same check JobManager/RetryClassifier performs before calling this
                    if (!everInserted.empty()) {
                        const std::string& id = everInserted[rng() % everInserted.size()];
                        const RetryBudget budget = LookUpRetryBudget(scheduler, id);
                        if (budget.attempt <= budget.maxRetries) {
                            scheduler.ScheduleRetry(id, 500, now, "transient");
                        }
                    }
                    break;
                case 6:  // reorder or reprioritize a pending job (no-op/throw on a live/finished
                        // one, which SchedulerCore itself is responsible for rejecting)
                    if (!everInserted.empty()) {
                        scheduler.Move(everInserted[rng() % everInserted.size()], directions[rng() % 4]);
                    }
                    break;
                case 7:
                    if (!everInserted.empty()) {
                        scheduler.SetPriority(everInserted[rng() % everInserted.size()],
                                              priorities[pickPriority(rng)]);
                    }
                    break;
                case 8:  // cancel any job this test still remembers -- SchedulerCore decides
                        // for itself whether that's legal from wherever it currently is
                    if (!everInserted.empty()) {
                        const std::string& id = everInserted[rng() % everInserted.size()];
                        scheduler.SetState(id, JobState::Cancelled, now);
                        removeFromDispatched(id);
                    }
                    break;
                default:
                    break;
            }
        } catch (const std::exception&) {
            // Expected for a genuine fraction of these: Move/SetPriority/ScheduleRetry on a
            // job that is not currently eligible for that operation throw a
            // MediaToolException, by design. A rejection is a correct outcome, not a test
            // failure -- what's being verified is that the scheduler's OWN internal state
            // stays consistent whether an operation succeeds or is refused.
        }

        if (i % 53 == 0) scheduler.ClearHistory(HistoryScope::All);

        if (i % 250 == 0) {
            ExpectConsistent(scheduler, "operation #" + std::to_string(i));
        }
    }

    ExpectConsistent(scheduler, "the full 5000-operation sequence");

    const auto stats = scheduler.Stats();
    EXPECT_GE(stats.running, 0);
    EXPECT_GE(stats.queued, 0);
    EXPECT_GE(stats.completed, 0);
    EXPECT_GE(stats.failed, 0);
    EXPECT_LE(static_cast<std::size_t>(stats.total), everInserted.size());
}

// A large dependency fan-out: one root, many direct dependents, verifying none of them
// dispatch before the root completes and all of them become dispatchable together once it
// does -- the shape of "a download feeding many downstream jobs" pushed to real volume.
TEST(SchedulerCoreStressTest, LargeDependencyFanOutReleasesAllDependentsTogether) {
    SchedulerCore::Config config;
    config.maxConcurrency = 500;  // no concurrency limit in the way of this specific check
    SchedulerCore scheduler(config);

    constexpr int kDependents = 300;
    scheduler.Insert(MakeRecord("root", JobPriority::Normal, JobType::Download), 0);

    for (int i = 0; i < kDependents; ++i) {
        JobRecord record = MakeRecord("dep-" + std::to_string(i), JobPriority::Normal, JobType::Conversion);
        record.dependencies = {"root"};
        scheduler.Insert(std::move(record), 0);
    }
    ExpectConsistent(scheduler, "300-way fan-out insertion");

    // Nothing but the root can be dispatched while it's outstanding.
    auto dispatchable = scheduler.SelectDispatchable(0);
    ASSERT_EQ(dispatchable.size(), 1u);
    EXPECT_EQ(dispatchable.front(), "root");

    scheduler.SetState("root", JobState::Running, 0);
    scheduler.SetState("root", JobState::Completed, 0);
    scheduler.ResolveDependencies(0);
    ExpectConsistent(scheduler, "resolving 300 dependents after the root completed");

    dispatchable = scheduler.SelectDispatchable(0);
    EXPECT_EQ(dispatchable.size(), static_cast<std::size_t>(kDependents))
        << "every dependent should have become dispatchable in the same pass";

    std::unordered_set<std::string> uniqueIds(dispatchable.begin(), dispatchable.end());
    EXPECT_EQ(uniqueIds.size(), dispatchable.size()) << "no dependent should be selected twice";
}

// A long dependency CHAIN (not a fan-out) -- A -> B -> C -> ... -- confirms the resolution
// mechanism doesn't assume a shallow graph and stays correct at real depth, AND that the
// bounded-history eviction (spec section 12: "a queue containing thousands of historical
// jobs must not make the application unusable") actually kicks in under real volume rather
// than just in the small hand-written case SchedulerCoreTest.cpp already covers.
TEST(SchedulerCoreStressTest, LongDependencyChainExecutesInOrderAndHistoryStaysBounded) {
    SchedulerCore::Config config;
    config.maxConcurrency = 50;
    config.historyLimit = 64;
    SchedulerCore scheduler(config);

    constexpr int kChainLength = 200;
    for (int i = 0; i < kChainLength; ++i) {
        JobRecord record = MakeRecord("chain-" + std::to_string(i), JobPriority::Normal, JobType::Conversion);
        if (i > 0) record.dependencies = {"chain-" + std::to_string(i - 1)};
        scheduler.Insert(std::move(record), 0);
    }
    ExpectConsistent(scheduler, "200-deep chain insertion");

    int completedThisRun = 0;
    for (int i = 0; i < kChainLength; ++i) {
        auto dispatchable = scheduler.SelectDispatchable(i);
        ASSERT_EQ(dispatchable.size(), 1u) << "only link " << i << " should be dispatchable";
        EXPECT_EQ(dispatchable.front(), "chain-" + std::to_string(i));
        scheduler.SetState(dispatchable.front(), JobState::Running, i);
        scheduler.SetState(dispatchable.front(), JobState::Completed, i);
        ++completedThisRun;
        scheduler.ResolveDependencies(i);
    }
    ExpectConsistent(scheduler, "running the full 200-deep chain to completion");

    // Every link genuinely ran and completed in order (the ASSERT above already proved the
    // "in order, one at a time" part) -- that count is independent of history bounding.
    EXPECT_EQ(completedThisRun, kChainLength);

    // But the scheduler is only configured to REMEMBER 64 of them at once: history beyond
    // the configured limit is evicted, by design, so the queue stays a bounded structure
    // no matter how many jobs a long-running session accumulates. This is the actual
    // "thousands of historical jobs must not make the application unusable" property.
    const auto stats = scheduler.Stats();
    EXPECT_LE(stats.completed, 64);
    EXPECT_GT(stats.completed, 0);
    EXPECT_LE(scheduler.Snapshot().size(), 64u);
}

}  // namespace
