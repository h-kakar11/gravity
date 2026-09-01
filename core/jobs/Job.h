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
// JobManager observes this Job via SetCallbacks(); it is the only intended subscriber in
// Phase 1, and a later integration pass bridges those callbacks into the EventBus.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/common/IClock.h"
#include "core/errors/ErrorInfo.h"
#include "core/jobs/JobStateMachine.h"
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

    // Scheduling priority (issue #17): higher runs before lower when both are Queued.
    // Set once by the caller before SubmitJob() -- JobManager reads it only to decide
    // queue order, never mutates it, so a plain atomic (no mutex_ involvement) is enough.
    // Ties keep FIFO order among jobs of equal priority.
    int Priority() const { return priority_; }
    void SetPriority(int priority) { priority_ = priority; }

    // Ids of jobs that must COMPLETE before this one may start (issue #17). Like
    // Priority(), set by the caller before SubmitJob() and never mutated afterwards --
    // JobManager hands the list to SchedulerCore at submission and the scheduler owns the
    // graph from then on. Guarded by mutex_ so a snapshot taken from an IPC thread while a
    // worker runs the job is still well-defined.
    std::vector<JobId> DependsOn() const;
    void SetDependsOn(std::vector<JobId> dependsOn);

    // How many times Execute() has been entered for this job, including the run in
    // progress -- 0 before the first, 1 while the first is running. Incremented by the
    // one transition every attempt goes through (MarkRunning), so it counts attempts
    // rather than transitions.
    int AttemptCount() const { return attemptCount_; }

    // How long the CURRENT attempt has been running, or nullopt if this job is not
    // running. steady_clock, so a wall-clock correction cannot make a job look hours old;
    // StartedAt() stays the human-readable timestamp and is not a substitute, because it
    // is a system_clock ISO string and is not reset per attempt.
    std::optional<std::chrono::steady_clock::duration> RunningFor() const;
    // Seeds the counter for a job rebuilt after a crash, so a restart does not hand it a
    // fresh retry budget and let a permanently-broken job retry forever, three at a time,
    // for as long as the user keeps relaunching. Set before submission; never afterwards.
    void SetAttemptCount(int attempts) { attemptCount_ = attempts; }

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

    // Hooks subclasses may override to react to a request (e.g. to signal an underlying
    // ffmpeg process). Called from whichever thread issues the request; default no-ops.
    virtual void OnCancel() {}
    virtual void OnPause() {}
    virtual void OnResume() {}

    // --- Lifecycle, driven by JobManager --------------------------------------------
    // Requests cancellation. Safe to call from any thread, any number of times, in any
    // state. If the job is still Queued -- meaning no worker thread will ever call
    // Execute() to notice the flag -- transitions directly to Cancelled and returns
    // Success. Otherwise only sets the flag/wakes a paused wait and returns Success: the
    // worker thread finalizes the Cancelled transition once Execute() throws or returns.
    // Returns AlreadyTerminal if the job had already finished (of which "already
    // Cancelled" is the idempotent case callers normally ignore) -- cancelling twice, or
    // cancelling a job that just completed, is a normal outcome, never an error.
    TransitionResult RequestCancel();

    // Returns InvalidTransition (and does nothing) unless SupportsPause() and currently
    // Running; AlreadyInState if already Paused.
    TransitionResult RequestPause();
    // Returns InvalidTransition (and does nothing) unless currently Paused;
    // AlreadyInState if already Running.
    TransitionResult RequestResume();

    bool IsCancellationRequested() const { return cancellationRequested_; }

    // JobManager calls these around Execute() to drive the state machine. None of them
    // throws: a transition that cannot happen is reported through the returned
    // TransitionResult (see core/jobs/JobStateMachine.h) so a routine race -- typically a
    // cancellation landing between a worker thread's State() read and its next Mark* call
    // -- is a value the caller inspects rather than an exception crossing a thread
    // boundary. They are idempotent in the useful sense: calling MarkCancelled() on an
    // already-Cancelled job returns AlreadyInState and changes nothing.
    TransitionResult MarkStarting();
    TransitionResult MarkRunning();
    TransitionResult MarkCompleted();
    // On Success the job's error is set to `error`; on any other result `error` is
    // discarded and the existing terminal outcome stands.
    TransitionResult MarkFailed(errors::ErrorInfo error);
    TransitionResult MarkCancelled();
    // Only valid from Failed. Clears the cancellation flag so a fresh run starts clean.
    // This is the MANUAL retry path (a user pressing Retry on a job that gave up).
    TransitionResult MarkRetrying();

    // The AUTOMATIC retry path: records `error` as the most recent attempt's failure and
    // moves RUNNING -> RETRYING directly, without the job ever being terminal. See
    // JobStateMachine.cpp for why that distinction is load-bearing rather than cosmetic.
    // Clears the cancellation flag, same as MarkRetrying.
    TransitionResult MarkRetryScheduled(errors::ErrorInfo error);

    // There is deliberately no public "can this job transition to X?" query. Answering it
    // and then acting on the answer is the check-then-act pattern the #4 race lived in --
    // the state can change in between, and only the Mark* call that actually moves the job
    // knows what really happened. Callers attempt the transition and inspect the result.
    // The pure predicate over the transition table itself lives in
    // core/jobs/JobStateMachine.h (CanTransition), where the table is defined.

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
    // The single unchecked commit point every transition funnels through. Classifies the
    // attempt (see TransitionResult) and, on Success only, applies `to`. Caller must hold
    // mutex_; callbacks are deliberately NOT fired here, so the caller can release the
    // lock first.
    TransitionResult TransitionLocked(JobState to);
    void FireStateChanged(JobState state);
    void FireProgress(const Progress& progress);

    const JobId id_;
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
    std::atomic<int> priority_{0};
    // Atomic rather than mutex_-guarded for the same reason priority_ is: it is read from
    // JobManager's retry decision on a worker thread while an IPC thread may be building
    // a snapshot, and it needs no consistency with any other field.
    std::atomic<int> attemptCount_{0};
    // Set on each transition into Running; guarded by mutex_ alongside state_ so "is it
    // running, and since when" is answered from one consistent read.
    std::optional<std::chrono::steady_clock::time_point> runningSince_;
    std::vector<JobId> dependsOn_;

    StateChangedCallback onStateChanged_;
    ProgressCallback onProgress_;
};

}  // namespace mediatool::jobs
