#pragma once

// Job orchestration (spec section 6). Owns every submitted Job, runs them on a small
// fixed worker-thread pool sized by `maxConcurrentJobs` (constructor parameter, NOT
// hardcoded -- spec section 6 requires this to scale to N later even though Phase 1
// callers may pass 1), and exposes subscription points
// (OnJobStateChanged/OnJobProgress) that a later integration pass bridges into the
// EventBus this module does not own or know about.
//
// Concurrency model (docs/concurrency-model.md): N worker threads, each asking
// SchedulerCore for the next eligible job, running it to completion, and asking again.
// The pool size alone caps concurrent execution at N without needing a separate "running"
// counter.
//
// What runs next is not decided here. JobManager owns threads, locks, ownership of Job
// objects and the callback plumbing; SchedulerCore (core/jobs/SchedulerCore.h) owns
// priority, dependencies and eligibility, with no threads of its own. JobManager calls
// into it under mutex_ and never lets a Job callback fire while holding that lock.

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

#include "core/errors/ErrorInfo.h"
#include "core/jobs/Job.h"
#include "core/jobs/JobStateMachine.h"
#include "core/jobs/JobTypes.h"
#include "core/jobs/Progress.h"
#include "core/jobs/RetryPolicy.h"
#include "core/jobs/SchedulerCore.h"

namespace mediatool::jobs {

class JobManager {
public:
    using JobStateChangedCallback = std::function<void(const JobId&, JobState)>;
    using JobProgressCallback = std::function<void(const JobId&, const Progress&)>;

    // Read-only point-in-time view of a Job, e.g. for getJob/listJobs
    // (docs/ipc-contract.md "Job snapshot"). Built from several independent Job getter
    // calls rather than one atomic Job-side snapshot method -- a deliberate Phase 1
    // simplification; a field or two could in theory be a moment stale relative to each
    // other under concurrent mutation, which is acceptable for status-reporting use.
    struct JobSnapshot {
        JobId id;
        JobType type;
        JobState state;
        int priority = 0;
        // Attempts that have already run, including the one in progress. 1 for a job on
        // its first run; 2 while it is retrying after one failure. Surfaced so the UI can
        // say "attempt 2 of 3" instead of silently re-running a job the user watched fail.
        int attempts = 0;
        Progress progress;
        std::optional<errors::ErrorInfo> error;
        std::optional<nlohmann::json> result;
        nlohmann::json metadata;
        std::string createdAt;
        std::optional<std::string> startedAt;
        std::optional<std::string> completedAt;

        // Matches the "Job snapshot" JSON shape in docs/ipc-contract.md exactly.
        nlohmann::json ToJson() const;
    };

    // Starts a pool of `maxConcurrentJobs` worker threads (0 is treated as 1). If the OS
    // refuses to create that many threads, the pool is silently smaller -- see
    // MaxConcurrentJobs() -- rather than the process dying on a partially constructed
    // pool. Throws errors::MediaToolException{EngineFailure, "E_WORKER_POOL_UNAVAILABLE"}
    // only if not one worker could be started, since a JobManager that can never run
    // anything is not worth handing back.
    // `retryPolicy` governs AUTOMATIC retry of failed jobs; the default retries a
    // recoverable failure twice with exponential backoff. Pass `RetryPolicy{.maxAttempts
    // = 1}` to disable it entirely, which is what a test that wants a failure to stay
    // failed should do.
    explicit JobManager(std::size_t maxConcurrentJobs = 1, RetryPolicy retryPolicy = RetryPolicy{});
    ~JobManager();

    JobManager(const JobManager&) = delete;
    JobManager& operator=(const JobManager&) = delete;

    // Cancels every still-Queued job (finalized synchronously, no worker needed) and
    // requests cancellation of every currently-Running job, *before* waking/joining the
    // worker pool -- so shutdown only ever waits on work already in flight, never on
    // newly-started queued work (#6: previously the destructor let workers keep pulling
    // and starting fresh jobs off the queue while it waited for all of them to run to
    // completion). Safe to call more than once; the destructor calls this too, so an
    // explicit prior call just makes it a no-op there. Does not itself impose a timeout --
    // a Running job that never checks IsCancellationRequested() still blocks shutdown
    // until its own Execute() returns.
    void Shutdown();

    // The number of worker threads that actually exist, which is what bounds concurrency.
    // May be lower than the constructor's argument (see above).
    std::size_t MaxConcurrentJobs() const { return maxConcurrentJobs_; }

    // Registers `job` and enqueues it for execution on the worker pool. Ownership of
    // `job` passes to the JobManager. Returns its JobId (job->Id()).
    //
    // Reads the job's Priority() and DependsOn() once, here, and hands them to
    // SchedulerCore. Throws (and does not register the job) if the scheduler refuses the
    // submission -- e.g. a dependency naming a job that does not exist or has already
    // failed; see SchedulerCore::Submit.
    JobId SubmitJob(std::unique_ptr<Job> job);

    // Throws errors::MediaToolException (ErrorCategory::Unknown, code
    // "E_JOB_NOT_FOUND") if `id` is unknown.
    JobSnapshot GetJob(const JobId& id) const;
    std::vector<JobSnapshot> ListJobs() const;

    // All four below throw errors::MediaToolException if `id` is unknown, or if the
    // requested operation is not valid for the job's current state/type -- this mirrors
    // the "throw an honest error rather than silently no-op" Phase 1 convention.
    void CancelJob(const JobId& id);
    void PauseJob(const JobId& id);
    void ResumeJob(const JobId& id);
    // Only valid from Failed. The MANUAL retry a user asks for -- unlike an automatic
    // one, it ignores the policy's attempt limit, because a person who presses Retry has
    // decided something the policy could not know.
    void RetryJob(const JobId& id);

    const RetryPolicy& GetRetryPolicy() const { return retryPolicy_; }

    // Drops a job in a terminal state (Completed/Failed/Cancelled) from the active set.
    // Throws if `id` is unknown, the job is not yet terminal, or a still-queued job
    // depends on it (E_JOB_HAS_DEPENDENTS -- removing it would leave that job waiting on
    // an outcome nothing can report).
    void RemoveJob(const JobId& id);

    // Replaces any previously-registered subscriber (not additive). Called from
    // whichever thread is running the job at the time of the change.
    void OnJobStateChanged(JobStateChangedCallback callback);
    void OnJobProgress(JobProgressCallback callback);

    // Testing seam only -- never set outside tests. If set, RunJob() calls this
    // synchronously on the worker thread immediately after observing a Queued job and
    // before calling MarkStarting() on it. This is exactly the window in which a
    // concurrent RequestCancel() can transition the job straight to Cancelled, making
    // MarkStarting() report AlreadyTerminal (the #4 race, which used to throw here) -- a
    // hook lets a test force that interleaving deterministically instead of depending on
    // unreliable timing.
    void SetPreMarkStartingHookForTesting(std::function<void(const JobId&)> hook);

private:
    void WorkerLoop();
    void RunJob(const JobId& id);
    // Runs the testing-only interleaving hook, then attempts this job's QUEUED ->
    // STARTING claim, returning that attempt's result.
    TransitionResult RunPreMarkStartingHook(const JobId& id);
    // True if `result` means this worker owns the job and may proceed. Logs (but does not
    // throw) when the result indicates a state-machine disagreement rather than a lost
    // race -- see the TransitionResult documentation in core/jobs/JobStateMachine.h.
    static bool ClaimedForExecution(const Job& job, TransitionResult result,
                                     JobState attempted = JobState::Running);
    // Records a failed attempt and decides what happens next: either the job is left
    // FAILED, or -- when the policy says the failure is worth another attempt -- it goes
    // RUNNING -> RETRYING (never through FAILED, see JobStateMachine.cpp) and is requeued
    // behind a backoff.
    void FinalizeFailure(Job& job, const errors::ErrorInfo& error);
    Job* LookupJobLocked(const JobId& id) const;
    // Cancels jobs the scheduler reported as unrunnable because a dependency did not
    // complete. Must be called with mutex_ released: cancelling fires state-changed
    // callbacks that re-enter this class.
    void CancelStrandedDependents(const std::vector<JobId>& ids, const JobId& because);
    JobSnapshot SnapshotOf(const Job& job) const;
    void HandleJobStateChanged(const JobId& id, JobState state);
    void HandleJobProgress(const JobId& id, const Progress& progress);
    [[noreturn]] void ThrowNotFound(const JobId& id) const;
    [[noreturn]] void ThrowInvalidOperation(const JobId& id, const std::string& reason) const;

    // Not const: fixed up in the constructor body once the pool's real size is known.
    std::size_t maxConcurrentJobs_ = 0;
    // Immutable after construction, so it needs no lock.
    RetryPolicy retryPolicy_;

    mutable std::mutex mutex_;
    std::map<JobId, std::unique_ptr<Job>> jobs_;
    // Guarded by mutex_ -- SchedulerCore is deliberately not thread-safe on its own, so
    // that its logic stays testable without threads and there is exactly one lock in this
    // subsystem rather than two that could be taken in two orders.
    SchedulerCore scheduler_;
    std::condition_variable queueCv_;
    bool stopping_ = false;

    JobStateChangedCallback stateChangedCallback_;
    JobProgressCallback progressCallback_;
    std::function<void(const JobId&)> preMarkStartingHookForTesting_;

    std::vector<std::thread> workers_;
};

}  // namespace mediatool::jobs
