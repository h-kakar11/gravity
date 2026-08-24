#pragma once

// The queue's decision engine: what runs next, in what order, and when.
//
// SchedulerCore is deliberately PURE and SINGLE-THREADED. It owns no threads, no locks, no
// Job objects, and never reads a clock -- every method that needs the current time takes it
// as a parameter. That is what makes the whole of Phase 5's scheduling behaviour (FIFO,
// priority, concurrency limits, fairness, reordering, dependency gating, retry backoff)
// testable as ordinary deterministic function calls, with no sleeps and no flakiness.
//
// JobManager is what wraps this in a mutex, owns the live Job objects, runs them on worker
// threads, and keeps each JobRecord's `state` in step with its Job's actual state. The
// division of responsibility is strict:
//
//     SchedulerCore  decides   -- which job, what order, is it allowed to start
//     JobManager     executes  -- threads, Job lifecycle, events, persistence
//
// Nothing in here launches a process or touches a file.
//
// Ordering model (spec sections 9, 10, 44). There is one pending order -- a list the user
// can reorder directly. Selection walks that list, keeps the entries that are eligible
// right now, and picks the best by:
//
//     1. effective priority, highest first  (base priority + fairness aging)
//     2. position in the pending order, earliest first  (so equal priority is FIFO,
//        and an explicit "move to top" actually means something)
//
// Fairness aging exists so a steady stream of HIGH work cannot starve a NORMAL job
// forever: a job that has been pending for a while gains rank in bounded steps until it
// can compete. It is capped, so aging can lift a job past higher tiers but never turns
// priority into noise.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/errors/ErrorInfo.h"
#include "core/queue/JobRecord.h"
#include "core/queue/QueueTypes.h"

namespace mediatool::queue {

// A dependency-driven state change the scheduler has decided on. JobManager applies these
// to the live Job objects (SchedulerCore cannot -- it has no access to them).
struct PendingTransition {
    jobs::JobId id;
    jobs::JobState newState = jobs::JobState::Queued;
    // Populated when newState is Skipped: which dependency killed it and why.
    std::optional<errors::ErrorInfo> reason;
};

class SchedulerCore {
public:
    struct Config {
        // Hard ceiling on jobs executing at once. Enforced centrally here, so it caps real
        // running work rather than merely the number of job objects (spec section 7).
        std::size_t maxConcurrency = 2;
        // Every this-many milliseconds a pending job waits, it gains one rank of priority.
        // 0 disables aging entirely (strict priority, starvation possible).
        std::int64_t agingIntervalMs = 30'000;
        // Ceiling on aging's contribution, in rank units. PriorityRank spaces tiers 10
        // apart, so the default lets a starved job climb two full tiers and no further.
        int maxAgingBoost = 20;
        // How many terminal jobs to keep as history before evicting the oldest
        // (spec section 26).
        std::size_t historyLimit = 100;
    };

    SchedulerCore() = default;
    explicit SchedulerCore(Config config) : config_(config) {}

    // --- configuration ---------------------------------------------------------------
    const Config& GetConfig() const { return config_; }
    void SetMaxConcurrency(std::size_t value);
    void SetHistoryLimit(std::size_t value) { config_.historyLimit = value; }
    QueueRunState RunState() const { return runState_; }
    void SetRunState(QueueRunState state) { runState_ = state; }

    // --- admission -------------------------------------------------------------------
    // Registers `record` and places it at the tail of the pending order. `record.state` is
    // set to Queued or Waiting depending on whether its dependencies are already satisfied.
    //
    // Throws errors::MediaToolException for: a duplicate id, an unknown dependency id, a
    // self-dependency, a dependency cycle, or a duplicate request (non-empty duplicateKey
    // matching a job that is still active). Nothing is inserted when it throws.
    void Insert(JobRecord record, std::int64_t nowMs);

    // True if a still-active job already carries `key`; returns its id. Lets the IPC layer
    // report the existing job rather than making the caller catch an exception.
    std::optional<jobs::JobId> FindActiveDuplicate(const std::string& key) const;

    // --- state synchronisation --------------------------------------------------------
    // Records that a job's live state changed, updating pending-set membership, running
    // counts, finish timestamps, and history. Safe to call with the same state twice.
    void SetState(const jobs::JobId& id, jobs::JobState state, std::int64_t nowMs);

    // --- scheduling -------------------------------------------------------------------
    // Ids that should start right now, in the order they should start, honouring the
    // concurrency limit and the queue's run state. Marks each returned job as Starting
    // internally so two consecutive calls cannot hand out the same job twice -- the caller
    // MUST drive every returned id (or report its real state back via SetState).
    std::vector<jobs::JobId> SelectDispatchable(std::int64_t nowMs);

    // Dependency-driven transitions that are due: Waiting -> Queued once every dependency
    // has completed, Waiting/Queued -> Skipped once any dependency has failed, been
    // cancelled, or been skipped itself. Applies them to its own records and returns them
    // so the caller can apply them to the live Jobs.
    std::vector<PendingTransition> ResolveDependencies(std::int64_t nowMs);

    // Earliest future instant at which SelectDispatchable could return something it cannot
    // return now -- i.e. the soonest retry deadline. nullopt when nothing is time-gated, in
    // which case the caller should sleep until it is explicitly woken.
    std::optional<std::int64_t> NextWakeupMs(std::int64_t nowMs) const;

    // --- retry ------------------------------------------------------------------------
    // Moves a failed job into RetryWait, eligible again at nowMs + delayMs, and counts the
    // attempt. Caller decides delay via BackoffDelayMs and whether a retry is warranted at
    // all via ClassifyRetry.
    void ScheduleRetry(const jobs::JobId& id, std::int64_t delayMs, std::int64_t nowMs,
                       std::string reason);

    // True if `id` has retry budget left under its own policy.
    bool HasRetryBudget(const jobs::JobId& id) const;

    // Queues a fresh attempt requested by the user: counts the attempt and places the job
    // in RetryWait with its deadline set to `nowMs`, i.e. eligible on the very next
    // dispatch. Deliberately reuses the automatic-retry path rather than adding a second
    // one -- see the implementation comment.
    void PrepareManualRetry(const jobs::JobId& id, std::int64_t nowMs);

    // --- ordering / priority ----------------------------------------------------------
    // Throws if `id` is unknown or is not currently pending (a running or finished job has
    // no queue position to change -- spec section 10).
    void Move(const jobs::JobId& id, MoveDirection direction);
    // Allowed in any non-terminal state. On a running job it takes effect only if the job
    // is later retried; it never preempts (spec section 9).
    void SetPriority(const jobs::JobId& id, JobPriority priority);

    // --- removal ----------------------------------------------------------------------
    // Removes terminal jobs matching `scope`. Never removes an active or pending job, and
    // never touches files on disk (spec section 27). Returns the removed ids.
    std::vector<jobs::JobId> ClearHistory(HistoryScope scope);
    // Drops a single terminal job. Throws if unknown or still active.
    void Remove(const jobs::JobId& id);

    // --- inspection -------------------------------------------------------------------
    bool Contains(const jobs::JobId& id) const { return records_.count(id) > 0; }
    const JobRecord* Find(const jobs::JobId& id) const;
    JobRecord* FindMutable(const jobs::JobId& id);
    // Every record, ordered: pending jobs first in queue order, then everything else by
    // creation sequence. This is the order the frontend renders by default.
    std::vector<JobRecord> Snapshot() const;
    const std::vector<jobs::JobId>& PendingOrder() const { return pendingOrder_; }
    std::size_t RunningCount() const;
    QueueStatistics Stats() const;
    std::int64_t NextSequence() { return ++sequenceCounter_; }
    // Ids that list `id` among their dependencies.
    std::vector<jobs::JobId> DependentsOf(const jobs::JobId& id) const;

    // --- invariants -------------------------------------------------------------------
    // Human-readable descriptions of any broken invariant (spec section 45). Empty means
    // the scheduler is internally consistent. Tests assert this after every operation; it
    // is cheap enough to call from a debug build but is not on the hot path.
    std::vector<std::string> ValidateInvariants() const;

private:
    bool IsPendingState(jobs::JobState state) const;
    void AddToPending(const jobs::JobId& id);
    void RemoveFromPending(const jobs::JobId& id);
    bool IsEligible(const JobRecord& record, std::int64_t nowMs) const;
    int EffectivePriority(const JobRecord& record, std::int64_t nowMs) const;
    // Throws if adding `record`'s dependency edges would create a cycle.
    void RejectCycles(const JobRecord& record) const;
    void RecordTerminal(const jobs::JobId& id);
    void EnforceHistoryLimit();

    Config config_;
    QueueRunState runState_ = QueueRunState::Running;

    std::map<jobs::JobId, JobRecord> records_;
    // User-visible queue order. Holds exactly the ids whose state is pending
    // (Queued/Waiting/RetryWait) -- an invariant ValidateInvariants() checks.
    std::vector<jobs::JobId> pendingOrder_;
    // Terminal jobs in the order they finished, for bounded history eviction.
    std::vector<jobs::JobId> historyOrder_;
    std::int64_t sequenceCounter_ = 0;
};

}  // namespace mediatool::queue
