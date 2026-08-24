#pragma once

// Abstract job base (spec sections 4-6). A Job owns its own state machine (enforced via
// JobStateMachine::CanTransition) and is safe to read from a different thread than the
// one running Execute() -- JobManager runs Execute() on a worker thread while an IPC
// handler thread reads State()/GetProgress() concurrently for getJob/listJobs. All
// mutable state is guarded by mutex_; state-changed/progress callbacks are invoked
// outside the lock so a callback may safely call back into this Job (e.g. GetProgress()).
//
// Cancellation convention (mirrors IDownloadProvider/IMediaEngine): a running job that
// notices IsCancellationRequested() must stop by throwing
// errors::MediaToolException(ErrorCategory::Cancelled) from Execute() -- it must never
// return normally as if it had completed successfully. The one exception is a job
// cancelled before/while it never entered Execute() at all (still Queued, or parked in
// WaitWhilePaused before OnCancel woke it) -- see JobManager for how that's finalized.
//
// JobManager observes this Job via SetCallbacks() and is its only intended subscriber; it
// is what bridges these callbacks onto the EventBus and keeps the queue's own record of the
// job in step with it.

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "core/common/IClock.h"
#include "core/errors/ErrorInfo.h"
#include "core/jobs/JobTypes.h"
#include "core/jobs/Progress.h"

namespace mediatool::jobs {

class Job {
public:
    using StateChangedCallback = std::function<void(JobState)>;
    using ProgressCallback = std::function<void(const Progress&)>;

    // Uses an internally-owned SystemClock.
    explicit Job(JobType type);
    // `clock` must outlive this Job. Lets tests inject a fixed/fake clock.
    Job(JobType type, common::IClock& clock);

    virtual ~Job() = default;

    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;

    // --- Identity / static properties -------------------------------------------------
    const JobId& Id() const { return id_; }
    JobType Type() const { return type_; }

    // Adopts an id that already exists elsewhere -- specifically, the id a job was
    // persisted under, so that a queue restored from disk after a restart keeps the ids
    // the user's UI and the state file already refer to. Rebuilding the Job necessarily
    // generates a fresh id in the constructor; this replaces it before anyone can observe
    // the new one.
    //
    // Only legal on a job that has never run: throws if the job has left Queued or has a
    // startedAt timestamp. This is not a general-purpose setter.
    void AdoptRestoredId(const JobId& id);

    // --- Thread-safe snapshots -----------------------------------------------------
    JobState State() const;
    Progress GetProgress() const;
    std::optional<errors::ErrorInfo> GetError() const;
    std::optional<nlohmann::json> GetResult() const;
    nlohmann::json GetMetadata() const;
    void SetMetadata(nlohmann::json metadata);
    std::string CreatedAt() const;
    std::optional<std::string> StartedAt() const;
    std::optional<std::string> CompletedAt() const;

    // Registers the observer JobManager uses to react to state/progress changes. Not
    // additive -- a second call replaces the previous callbacks. Callbacks may be called
    // from whatever thread drives this Job's execution.
    void SetCallbacks(StateChangedCallback onStateChanged, ProgressCallback onProgress);

    // --- The actual work -----------------------------------------------------------
    // Must periodically check IsCancellationRequested() and, upon seeing it true, throw
    // errors::MediaToolException(ErrorCategory::Cancelled) rather than returning as if
    // successful.
    virtual void Execute() = 0;

    virtual bool SupportsPause() const { return false; }

    // Supplies the input path this job should read, when that path was not knowable at
    // creation time because it is another job's output. Called by JobManager immediately
    // before Execute(), once the producing job has completed and its result is final.
    //
    // This exists so a pipeline can be declared up front rather than assembled by the
    // frontend polling for one job to finish and then guessing what it produced -- a guess
    // that cannot be right for a download, whose filename comes from the media's title and
    // whichever container the extractor chose (spec section 19). Default: no-op, for job
    // types whose input is fixed.
    virtual void ApplyResolvedInput(const std::string& /*inputPath*/) {}

    // Hooks subclasses may override to react to a request (e.g. to signal an underlying
    // ffmpeg process). Called from whichever thread issues the request; default no-ops.
    virtual void OnCancel() {}
    virtual void OnPause() {}
    virtual void OnResume() {}

    // --- Lifecycle, driven by JobManager --------------------------------------------
    // Requests cancellation. Safe to call from any thread, any number of times, in any
    // state (a no-op once already terminal). If the job is still Queued -- meaning no
    // worker thread will ever call Execute() to notice the flag -- transitions directly
    // to Cancelled. Otherwise only sets the flag/wakes a paused wait; the worker thread
    // finalizes the Cancelled transition once Execute() throws or returns.
    void RequestCancel();

    // No-op unless SupportsPause() and currently Running.
    void RequestPause();
    // No-op unless currently Paused.
    void RequestResume();

    bool IsCancellationRequested() const { return cancellationRequested_; }

    // JobManager calls these around Execute() to drive the state machine; each throws
    // errors::MediaToolException(ErrorCategory::Unknown) if the transition is invalid
    // for the job's current state (a JobManager bug, not a user-facing condition).
    void MarkStarting();
    void MarkRunning();
    void MarkCompleted();
    void MarkFailed(errors::ErrorInfo error);
    void MarkCancelled();
    // Only valid from Failed or RetryWait. Clears the cancellation flag so a fresh run
    // starts clean.
    void MarkRetrying();

    // Blocked on an unmet dependency. Valid from Queued or Skipped.
    void MarkWaiting();
    // Dependencies are all satisfied; the job may now be scheduled. Valid from Waiting.
    void MarkQueued();
    // A transient failure earned an automatic retry. Valid from Failed. `error` is kept
    // visible so the UI can show *why* a retry is pending.
    void MarkRetryWait();
    // A dependency failed or was cancelled, so this job will never run. `reason` is stored
    // as the job's error so the frontend can explain the skip.
    void MarkSkipped(errors::ErrorInfo reason);

protected:
    // Replaces the stored progress with `progress` and notifies the progress callback.
    void ReportProgress(Progress progress);
    void SetResult(nlohmann::json result);
    void SetError(errors::ErrorInfo error);

    // Checkpoint for subclasses whose work happens in discrete steps (e.g. TestJob's
    // sleep loop). Blocks while State() == Paused. Returns false if cancellation was
    // requested (either before or while waiting) -- the caller must then stop, normally
    // by throwing ErrorCategory::Cancelled same as any other cancellation checkpoint.
    bool WaitWhilePaused();

private:
    // Returns true if the transition happened. Caller must hold mutex_.
    bool TransitionLocked(JobState to);
    void FireStateChanged(JobState state);
    void FireProgress(const Progress& progress);
    [[noreturn]] void ThrowInvalidTransition(JobState from, JobState to) const;

    JobId id_;
    const JobType type_;

    std::unique_ptr<common::IClock> ownedClock_;
    common::IClock& clock_;

    mutable std::mutex mutex_;
    std::condition_variable pauseCv_;
    JobState state_ = JobState::Queued;
    Progress progress_{.statusMessage = "Queued"};
    std::optional<errors::ErrorInfo> error_;
    std::optional<nlohmann::json> result_;
    nlohmann::json metadata_ = nlohmann::json::object();
    std::string createdAt_;
    std::optional<std::string> startedAt_;
    std::optional<std::string> completedAt_;

    std::atomic<bool> cancellationRequested_{false};

    StateChangedCallback onStateChanged_;
    ProgressCallback onProgress_;
};

}  // namespace mediatool::jobs
