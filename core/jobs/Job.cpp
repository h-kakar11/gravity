#include "core/jobs/Job.h"

#include "core/jobs/JobStateMachine.h"

namespace mediatool::jobs {

// Not delegated to the (type, IClock&) constructor: a delegating constructor's
// mem-initializer-list may only contain the delegation call, so ownedClock_ couldn't be
// set up before clock_ needs to bind to it. Small duplication instead.
Job::Job(JobType type)
    : id_(GenerateJobId()),
      type_(type),
      ownedClock_(std::make_unique<common::SystemClock>()),
      clock_(*ownedClock_) {
    createdAt_ = clock_.NowIso8601Utc();
}

Job::Job(JobType type, common::IClock& clock)
    : id_(GenerateJobId()), type_(type), ownedClock_(nullptr), clock_(clock) {
    createdAt_ = clock_.NowIso8601Utc();
}

JobState Job::State() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

Progress Job::GetProgress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return progress_;
}

std::optional<errors::ErrorInfo> Job::GetError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

std::optional<nlohmann::json> Job::GetResult() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return result_;
}

nlohmann::json Job::GetMetadata() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metadata_;
}

void Job::SetMetadata(nlohmann::json metadata) {
    std::lock_guard<std::mutex> lock(mutex_);
    metadata_ = std::move(metadata);
}

std::string Job::CreatedAt() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return createdAt_;
}

std::optional<std::string> Job::StartedAt() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return startedAt_;
}

std::optional<std::string> Job::CompletedAt() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return completedAt_;
}

std::vector<JobId> Job::DependsOn() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dependsOn_;
}

void Job::SetDependsOn(std::vector<JobId> dependsOn) {
    std::lock_guard<std::mutex> lock(mutex_);
    dependsOn_ = std::move(dependsOn);
}

void Job::SetCallbacks(StateChangedCallback onStateChanged, ProgressCallback onProgress) {
    std::lock_guard<std::mutex> lock(mutex_);
    onStateChanged_ = std::move(onStateChanged);
    onProgress_ = std::move(onProgress);
}

void Job::FireStateChanged(JobState state) {
    StateChangedCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = onStateChanged_;
    }
    if (callback) callback(state);
}

void Job::FireProgress(const Progress& progress) {
    ProgressCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = onProgress_;
    }
    if (callback) callback(progress);
}

TransitionResult Job::TransitionLocked(JobState to) {
    // Order matters: "already there" and "already finished elsewhere" are both normal
    // outcomes under concurrency and must be distinguishable from a genuinely illegal
    // request, which is the only one of the three that indicates a bug. CanTransition()
    // rejects every self-transition, so these two cases have to be answered before
    // consulting it.
    if (state_ == to) return TransitionResult::AlreadyInState;
    if (IsTerminalState(state_)) return TransitionResult::AlreadyTerminal;
    if (!CanTransition(state_, to)) return TransitionResult::InvalidTransition;
    state_ = to;
    return TransitionResult::Success;
}

void Job::ReportProgress(Progress progress) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_ = progress;
    }
    FireProgress(progress);
}

void Job::SetResult(nlohmann::json result) {
    std::lock_guard<std::mutex> lock(mutex_);
    result_ = std::move(result);
}

void Job::SetError(errors::ErrorInfo error) {
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = std::move(error);
}

bool Job::WaitWhilePaused() {
    std::unique_lock<std::mutex> lock(mutex_);
    pauseCv_.wait(lock, [this] {
        return state_ != JobState::Paused || cancellationRequested_.load();
    });
    return !cancellationRequested_;
}

TransitionResult Job::RequestCancel() {
    bool transitionedHere = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Already finished (including already cancelled): nothing to do, and saying so is
        // not an error -- see TransitionResult::AlreadyTerminal.
        if (IsTerminalState(state_)) return TransitionResult::AlreadyTerminal;
        cancellationRequested_ = true;
        if (state_ == JobState::Queued) {
            // No worker thread will ever run Execute() for a still-Queued job, so
            // nothing else will ever notice the flag -- finalize the transition here.
            transitionedHere = TransitionLocked(JobState::Cancelled) == TransitionResult::Success;
            if (transitionedHere) completedAt_ = clock_.NowIso8601Utc();
        }
    }
    pauseCv_.notify_all();  // wake WaitWhilePaused() if a paused job is being cancelled
    OnCancel();
    if (transitionedHere) FireStateChanged(JobState::Cancelled);
    // Either the job is now Cancelled, or a worker thread has been told to stop and will
    // finalize it. Both are the caller's request being honored.
    return TransitionResult::Success;
}

TransitionResult Job::RequestPause() {
    if (!SupportsPause()) return TransitionResult::InvalidTransition;
    TransitionResult result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result = TransitionLocked(JobState::Paused);
    }
    if (result == TransitionResult::Success) {
        OnPause();
        FireStateChanged(JobState::Paused);
    }
    return result;
}

TransitionResult Job::RequestResume() {
    TransitionResult result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Guarded explicitly rather than leaning on the transition table: RUNNING is also
        // reachable from RETRYING, and a resumeJob call must never be able to drive a
        // retrying job into Running behind the worker thread's back.
        result = state_ == JobState::Paused ? TransitionLocked(JobState::Running)
                                            : (state_ == JobState::Running
                                                   ? TransitionResult::AlreadyInState
                                                   : TransitionResult::InvalidTransition);
    }
    if (result == TransitionResult::Success) {
        pauseCv_.notify_all();
        OnResume();
        FireStateChanged(JobState::Running);
    }
    return result;
}

TransitionResult Job::MarkStarting() {
    TransitionResult result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result = TransitionLocked(JobState::Starting);
    }
    if (result == TransitionResult::Success) FireStateChanged(JobState::Starting);
    return result;
}

TransitionResult Job::MarkRunning() {
    TransitionResult result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result = TransitionLocked(JobState::Running);
        if (result == TransitionResult::Success && !startedAt_) startedAt_ = clock_.NowIso8601Utc();
    }
    if (result == TransitionResult::Success) FireStateChanged(JobState::Running);
    return result;
}

TransitionResult Job::MarkCompleted() {
    TransitionResult result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result = TransitionLocked(JobState::Completed);
        if (result == TransitionResult::Success) completedAt_ = clock_.NowIso8601Utc();
    }
    if (result == TransitionResult::Success) FireStateChanged(JobState::Completed);
    return result;
}

TransitionResult Job::MarkFailed(errors::ErrorInfo error) {
    TransitionResult result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result = TransitionLocked(JobState::Failed);
        if (result == TransitionResult::Success) {
            error_ = std::move(error);
            completedAt_ = clock_.NowIso8601Utc();
        }
    }
    if (result == TransitionResult::Success) FireStateChanged(JobState::Failed);
    return result;
}

TransitionResult Job::MarkCancelled() {
    TransitionResult result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result = TransitionLocked(JobState::Cancelled);
        if (result == TransitionResult::Success) completedAt_ = clock_.NowIso8601Utc();
    }
    if (result == TransitionResult::Success) FireStateChanged(JobState::Cancelled);
    return result;
}

TransitionResult Job::MarkRetrying() {
    TransitionResult result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Retrying is the one legal way out of a terminal state, so it cannot go through
        // TransitionLocked's AlreadyTerminal short-circuit -- consult the table directly.
        if (state_ == JobState::Retrying) {
            result = TransitionResult::AlreadyInState;
        } else if (CanTransition(state_, JobState::Retrying)) {
            state_ = JobState::Retrying;
            cancellationRequested_ = false;
            completedAt_.reset();
            result = TransitionResult::Success;
        } else {
            result = TransitionResult::InvalidTransition;
        }
    }
    if (result == TransitionResult::Success) FireStateChanged(JobState::Retrying);
    return result;
}

}  // namespace mediatool::jobs
