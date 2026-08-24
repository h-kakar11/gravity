#pragma once

// The unified media-job orchestrator. There is exactly ONE queue in this application, and
// this is it: downloads, conversions and compressions are all ordinary jobs in it, and
// there is deliberately no separate download queue / conversion queue / compression queue.
//
// This class is the Phase 1 JobManager grown up, not a replacement bolted on beside it. Its
// public surface from Phase 1 (SubmitJob/GetJob/ListJobs/Cancel/Pause/Resume/Retry/Remove
// and the two subscription points) is intact and behaves the same way; what changed is the
// machinery underneath, plus the queue operations added at the bottom. Layering a second
// orchestrator on top of the old one was considered and rejected -- it would have produced
// exactly the competing-queues problem this phase exists to avoid, with jobs pending in one
// structure while a second one also thought it owned them. See docs/decisions.md.
//
// Responsibility split:
//
//   queue::SchedulerCore  decides   -- ordering, priority, concurrency, dependencies,
//                                      retry timing. Pure, deterministic, no threads.
//   JobManager (this)     executes  -- owns the Job objects, runs them on worker threads,
//                                      keeps records in sync, emits events, persists.
//
// Threading model:
//
//   * One scheduler thread. It is the only thread that dispatches work. It wakes on a
//     condition variable (or on the next retry deadline) and, per pass: resolves dependency
//     transitions, asks SchedulerCore what may start, launches a worker per dispatched job,
//     reaps finished workers, flushes coalesced progress, and persists if dirty.
//   * One worker thread per RUNNING job, created on dispatch and joined by the scheduler
//     thread once it finishes. Concurrency is enforced by SchedulerCore's admission
//     decision, NOT by the size of a thread pool -- which is what lets the limit change at
//     runtime and what makes it a cap on real running processes rather than on job objects.
//   * `mutex_` guards the scheduler, the job map, and all bookkeeping. It is NEVER held
//     while calling into a Job (Job methods fire callbacks that re-enter this class) and
//     never while joining a worker thread (which may be blocked entering this class).
//
// Job objects are held by shared_ptr: a worker thread keeps its job alive for the duration
// of the run, so clearing history or removing a job can never pull the object out from
// under the thread executing it.

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/common/IClock.h"
#include "core/errors/ErrorInfo.h"
#include "core/jobs/Job.h"
#include "core/jobs/JobTypes.h"
#include "core/jobs/Progress.h"
#include "core/queue/JobRecord.h"
#include "core/queue/QueuePersistence.h"
#include "core/queue/QueueTypes.h"
#include "core/queue/SchedulerCore.h"

namespace mediatool::jobs {

// Builds a runnable Job from a persisted JobSpec. The queue needs this to rebuild jobs
// after a restart: a JobRecord on disk describes what to do, and something has to know how
// to turn "DOWNLOAD plus these params" back into a DownloadJob wired to the real provider,
// filesystem and engine. app/core/main.cpp supplies the production implementation; tests
// supply a fake one and so can exercise the whole queue without any real media stack.
class IJobFactory {
public:
    virtual ~IJobFactory() = default;

    // Throws errors::MediaToolException if `spec` cannot be built (e.g. a job type this
    // build does not implement, or params that no longer validate).
    virtual std::unique_ptr<Job> Create(const queue::JobSpec& spec) = 0;
};

class JobManager {
public:
    using JobStateChangedCallback = std::function<void(const JobId&, JobState)>;
    using JobProgressCallback = std::function<void(const JobId&, const Progress&)>;
    // Fired when queue-level state changes: pause/resume, concurrency, ordering, or any
    // change to the aggregate counts. Carries no payload -- the subscriber re-reads
    // QueueSnapshot(), which cannot drift from what the queue actually thinks.
    using QueueChangedCallback = std::function<void()>;
    // (jobId, attempt, delayMs, reason)
    using RetryScheduledCallback =
        std::function<void(const JobId&, int, std::int64_t, const std::string&)>;

    struct Options {
        std::size_t maxConcurrentJobs = 2;
        // Where durable queue state lives. Empty disables persistence entirely, which is
        // what most unit tests want.
        std::string stateFilePath;
        // Minimum gap between queue-state writes. Progress never triggers a write; only
        // durable changes mark the queue dirty, and those are then coalesced to this
        // interval (spec sections 51, 52).
        std::int64_t persistIntervalMs = 500;
        // Minimum gap between progress events for one job. Prevents an event storm from
        // ffmpeg/yt-dlp's sub-second progress output (spec section 28).
        std::int64_t progressIntervalMs = 200;
        queue::RetryPolicy defaultRetryPolicy;
        std::int64_t agingIntervalMs = 30'000;
        std::size_t historyLimit = 100;
    };

    // Everything needed to create a job, as the IPC layer received it.
    struct SubmitRequest {
        queue::JobSpec spec;
        queue::JobPriority priority = queue::JobPriority::Normal;
        std::vector<JobId> dependencies;
        std::optional<JobId> parentJobId;
        std::optional<queue::RetryPolicy> retryPolicy;  // falls back to the manager default
        // Empty means "do not duplicate-check this job".
        std::string duplicateKey;
        // When true, a duplicateKey collision is allowed through instead of rejected.
        bool allowDuplicate = false;
        nlohmann::json metadata = nlohmann::json::object();
    };

    // Point-in-time view of a Job, unified across every job type (spec section 5): the Job's
    // own execution state plus the scheduling state SchedulerCore holds for it. This is what
    // getJob/listJobs/getQueueSnapshot all return, so the frontend never has to understand
    // three different job shapes.
    struct JobSnapshot {
        JobId id;
        JobType type;
        JobState state;
        Progress progress;
        std::optional<errors::ErrorInfo> error;
        std::optional<nlohmann::json> result;
        nlohmann::json metadata;
        std::string createdAt;
        std::optional<std::string> startedAt;
        std::optional<std::string> completedAt;

        // Queue-side fields.
        queue::JobPriority priority = queue::JobPriority::Normal;
        int attempt = 0;
        int maxRetries = 0;
        std::optional<std::int64_t> nextRetryAtMs;
        std::string lastRetryReason;
        std::vector<JobId> dependencies;
        std::optional<JobId> parentJobId;
        // Position in the pending order, or nullopt when the job is not pending.
        std::optional<int> queuePosition;
        std::int64_t revision = 0;

        nlohmann::json ToJson() const;
    };

    struct QueueSnapshot {
        queue::QueueRunState runState = queue::QueueRunState::Running;
        std::size_t maxConcurrency = 1;
        queue::QueueStatistics statistics;
        std::vector<JobSnapshot> jobs;
        // Pending job ids in scheduling order. The frontend may sort its view however it
        // likes, but this is the authoritative order (spec section 34).
        std::vector<JobId> pendingOrder;
        // "This snapshot already reflects every event up to and including this sequence
        // number." Filled in by the IPC layer, which owns the event sequence (see
        // app/core/main.cpp) -- the queue itself has no notion of wire ordering.
        std::int64_t sequence = 0;

        nlohmann::json ToJson() const;
    };

    // `factory` may be nullptr, in which case restart recovery cannot rebuild jobs and
    // persisted records are dropped with a diagnostic. `clock` must outlive this manager.
    explicit JobManager(std::size_t maxConcurrentJobs = 1);
    JobManager(Options options, IJobFactory* factory);
    JobManager(Options options, IJobFactory* factory, common::IClock& clock);
    ~JobManager();

    JobManager(const JobManager&) = delete;
    JobManager& operator=(const JobManager&) = delete;

    std::size_t MaxConcurrentJobs() const;

    // --- job submission ---------------------------------------------------------------
    // Registers `job` with default scheduling settings. Kept for callers that already hold
    // a constructed Job and have nothing to say about priority or dependencies.
    JobId SubmitJob(std::unique_ptr<Job> job);
    // Registers `job` with the scheduling metadata in `request` (request.spec is used only
    // for persistence, since the Job already exists). Throws on a rejected duplicate,
    // unknown dependency, or dependency cycle.
    JobId SubmitJob(std::unique_ptr<Job> job, const SubmitRequest& request);

    // --- inspection --------------------------------------------------------------------
    // Throws errors::MediaToolException ("E_JOB_NOT_FOUND") if `id` is unknown.
    JobSnapshot GetJob(const JobId& id) const;
    std::vector<JobSnapshot> ListJobs() const;
    QueueSnapshot GetQueueSnapshot() const;

    // --- per-job control ---------------------------------------------------------------
    void CancelJob(const JobId& id);
    void PauseJob(const JobId& id);
    void ResumeJob(const JobId& id);
    // Valid from Failed, RetryWait (skips the remaining backoff), or Skipped (re-evaluates
    // dependencies). Throws otherwise.
    void RetryJob(const JobId& id);
    void SetJobPriority(const JobId& id, queue::JobPriority priority);
    void MoveJob(const JobId& id, queue::MoveDirection direction);

    // Drops a terminal job. Throws if unknown or still active.
    void RemoveJob(const JobId& id);

    // --- queue control -----------------------------------------------------------------
    void PauseQueue();
    void ResumeQueue();
    void SetMaxConcurrency(std::size_t value);
    // Returns the ids removed. Never deletes files from disk.
    std::vector<JobId> ClearHistory(queue::HistoryScope scope);
    // Retries every currently-failed job. Returns the ids retried.
    std::vector<JobId> RetryAllFailed();

    // --- subscriptions ------------------------------------------------------------------
    // Each replaces any previously-registered subscriber (not additive). Called from
    // whichever thread produced the change.
    void OnJobStateChanged(JobStateChangedCallback callback);
    void OnJobProgress(JobProgressCallback callback);
    void OnQueueChanged(QueueChangedCallback callback);
    void OnRetryScheduled(RetryScheduledCallback callback);

    // --- persistence / recovery -----------------------------------------------------------
    // Loads persisted state, applies restart recovery, and rebuilds jobs via the factory.
    // Call once, after construction and after registering callbacks, before Start().
    // Never throws: a state file that cannot be used yields an empty queue and a diagnostic.
    struct RecoveryReport {
        queue::LoadOutcome::Status status = queue::LoadOutcome::Status::NotPresent;
        int restoredJobs = 0;
        int interruptedJobs = 0;
        int unbuildableJobs = 0;
        std::string diagnostic;
        std::optional<std::string> quarantinedPath;
    };
    RecoveryReport RestoreFromDisk();

    // Starts the scheduler thread. Idempotent. Separate from the constructor so a caller can
    // restore state and attach subscribers before anything begins running.
    void Start();
    // Stops the scheduler, waits for in-flight jobs, and writes final state. Called by the
    // destructor; safe to call explicitly first.
    void Shutdown();

    // Forces a state write regardless of the throttle interval. No-op without persistence.
    void FlushPersistence();

    // Test seam: runs one scheduler pass synchronously on the calling thread. Only for tests
    // that want to drive scheduling without racing the scheduler thread; do not call while
    // the scheduler thread is running.
    void RunSchedulerPassForTesting();

    // Test seam: internal consistency of the underlying scheduler (spec section 45).
    std::vector<std::string> ValidateInvariants() const;

private:
    struct ProgressThrottle {
        std::int64_t lastEmittedMs = 0;
        std::optional<Progress> pending;
    };

    void SchedulerLoop();
    // One pass of dependency resolution + dispatch + reaping + flushing. Returns the
    // absolute time the scheduler should next wake, or nullopt to wait indefinitely.
    std::optional<std::int64_t> SchedulerPass();
    void RunJob(std::shared_ptr<Job> job, JobId id);
    void HandleJobStateChanged(const JobId& id, JobState state);
    void HandleJobProgress(const JobId& id, const Progress& progress);
    // Decides whether a just-failed job earns an automatic retry, and schedules it.
    void MaybeScheduleAutomaticRetry(const JobId& id);
    void ApplyDependencyTransitions(const std::vector<queue::PendingTransition>& transitions);
    void MarkDirty();
    void PersistIfDue(std::int64_t nowMs, bool force);
    queue::PersistedQueue BuildPersistedQueueLocked() const;
    JobSnapshot SnapshotLocked(const JobId& id) const;
    std::shared_ptr<Job> LookupLocked(const JobId& id) const;
    void NotifyQueueChanged();
    std::int64_t NowMs() const;
    void WakeScheduler();
    [[noreturn]] void ThrowNotFound(const JobId& id) const;
    [[noreturn]] void ThrowInvalidOperation(const JobId& id, const std::string& reason) const;

    Options options_;
    IJobFactory* factory_ = nullptr;
    std::unique_ptr<common::IClock> ownedClock_;
    common::IClock& clock_;
    std::unique_ptr<queue::QueuePersistence> persistence_;

    mutable std::mutex mutex_;
    queue::SchedulerCore scheduler_;
    std::map<JobId, std::shared_ptr<Job>> jobs_;
    std::map<JobId, std::thread> workers_;
    std::vector<JobId> finishedWorkers_;
    std::map<JobId, ProgressThrottle> progressThrottles_;
    bool dirty_ = false;
    std::int64_t lastPersistMs_ = 0;

    std::condition_variable schedulerCv_;
    std::thread schedulerThread_;
    bool started_ = false;
    bool stopping_ = false;

    JobStateChangedCallback stateChangedCallback_;
    JobProgressCallback progressCallback_;
    QueueChangedCallback queueChangedCallback_;
    RetryScheduledCallback retryScheduledCallback_;
};

}  // namespace mediatool::jobs
