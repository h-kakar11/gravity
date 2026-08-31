#pragma once

// The scheduling policy, extracted from JobManager and made threadless on purpose
// (issue #17).
//
// JobManager previously *was* the scheduler: a std::deque<JobId> that a worker popped the
// front of, with priority bolted on as an insertion-order tweak. That conflates two
// unrelated things -- deciding what should run next, which is pure logic over a set of
// facts, and running it, which is threads, locks and callbacks. The consequence was that
// every scheduling question ("does priority actually order the queue?", "what happens to a
// job whose dependency fails?") could only be answered by starting threads and hoping the
// interleaving was the interesting one.
//
// So this class owns the decision and nothing else. It has no mutex, no condition
// variable, no threads and no knowledge of Job: it is a set of ids, priorities and
// dependency edges, and every method is a synchronous function of that state. JobManager
// holds it under its own lock and remains the only thing that touches threads. That makes
// the policy exhaustively testable without a single std::thread -- see
// tests/core/SchedulerCoreTest.cpp -- and keeps the concurrency surface confined to one
// class.
//
// Ordering: highest priority first, and among equal priorities, submission order (FIFO).
// A job with unmet dependencies is skipped, not blocking: a lower-priority job whose
// dependencies are met runs ahead of a higher-priority one still waiting.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/jobs/JobTypes.h"

namespace mediatool::jobs {

class SchedulerCore {
public:
    struct Submission {
        JobId id;
        int priority = 0;
        // Ids of jobs that must COMPLETE before this one may start. Every id must already
        // be known to the scheduler -- see Submit().
        std::vector<JobId> dependsOn;
    };

    // Registers a job as pending.
    //
    // Throws errors::MediaToolException{Unknown, "E_INVALID_DEPENDENCY"} if a dependency is
    // unknown, is the job itself, or has already finished in a non-COMPLETED state (a job
    // that could never run is worth refusing at submission, where the caller still has
    // somewhere to put the error, rather than accepting and immediately cancelling it).
    // Throws {Unknown, "E_DUPLICATE_JOB"} if `id` is already known.
    //
    // Requiring dependencies to exist before they can be depended on is also what makes a
    // dependency cycle impossible rather than merely detected: every edge points backwards
    // in submission order, so the graph is acyclic by construction. An attempt to build
    // A -> B -> A fails on the second submission, since A cannot name a job submitted after
    // it. This is enforced, not assumed -- see the cycle tests.
    void Submit(Submission submission);

    // The next job that may start: the highest-priority pending job all of whose
    // dependencies have COMPLETED. Marks it running and removes it from the pending set.
    // Returns nullopt if nothing is eligible, which is not the same as "nothing is pending"
    // -- jobs may be waiting on dependencies.
    std::optional<JobId> TakeNextEligible();

    // Whether TakeNextEligible() would return a job right now. For a worker's wait
    // predicate; inherently a snapshot.
    bool HasEligible() const;

    // Records that `id` finished in `terminal`, and returns the ids of pending jobs that
    // can now never run because they depend on it and it did not complete.
    //
    // Only direct dependents are returned. The caller finalizes each of them, which
    // produces its own terminal notification, which returns *its* dependents -- so a chain
    // unwinds one link at a time through the same path rather than needing a traversal
    // here. Calling this twice for the same job is a no-op the second time.
    std::vector<JobId> RecordTerminal(const JobId& id, JobState terminal);

    // Returns a known, finished job to the pending set (a retry). Its dependency edges are
    // unchanged: they completed once and stay completed. Throws E_JOB_NOT_FOUND if `id` is
    // unknown; a no-op if it is already pending.
    void Requeue(const JobId& id, int priority);

    // Drops everything the scheduler knows about `id`. Throws
    // {Unknown, "E_JOB_HAS_DEPENDENTS"} if a pending job depends on it -- forgetting it
    // would leave that job waiting on an id no one can ever report an outcome for.
    void Forget(const JobId& id);

    // Empties the pending set and returns what was in it, in scheduling order (shutdown:
    // the caller cancels them all). Jobs already running are untouched.
    std::vector<JobId> TakeAllPending();

    bool Knows(const JobId& id) const;
    bool IsPending(const JobId& id) const;
    std::size_t PendingCount() const;
    // Pending ids in the exact order they would be taken, ignoring dependency readiness.
    // Diagnostics and tests.
    std::vector<JobId> PendingOrder() const;

private:
    enum class Phase { Pending, Running, Finished };

    // Scheduling order as a sort key: higher priority first, then submission order. Used
    // as a std::map key so the pending set is *stored* in scheduling order rather than
    // sorted or scanned on every access -- with a queue of a few thousand jobs (a batch
    // drop), an O(n) insert that also does a lookup per element turns enqueuing into
    // quadratic work while JobManager's lock is held.
    struct PendingKey {
        int priority = 0;
        std::uint64_t sequence = 0;

        bool operator<(const PendingKey& other) const {
            if (priority != other.priority) return priority > other.priority;
            return sequence < other.sequence;
        }
    };

    struct Entry {
        PendingKey key;  // also how a pending entry is found for removal
        std::vector<JobId> dependsOn;
        Phase phase = Phase::Pending;
        JobState terminal = JobState::Queued;  // meaningful only when phase == Finished
    };

    // True if every dependency of `id` has finished as COMPLETED.
    bool DependenciesSatisfied(const JobId& id) const;

    std::map<JobId, Entry> entries_;
    std::map<PendingKey, JobId> pending_;             // iterates in scheduling order
    std::map<JobId, std::vector<JobId>> dependents_;  // reverse edges of dependsOn
    std::uint64_t nextSequence_ = 0;
};

}  // namespace mediatool::jobs
