#include "core/jobs/Job.h"

#include "core/errors/MediaToolException.h"
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

void Job::ThrowInvalidTransition(JobState from, JobState to) const {
    throw errors::MediaToolException(errors::ErrorInfo::Make(
        "E_INVALID_JOB_TRANSITION", errors::ErrorCategory::Unknown,
        "Internal error: invalid job state transition",
        "Job " + id_ + " cannot transition from " + ToWireString(from) + " to " +
            ToWireString(to)));
}

bool Job::TransitionLocked(JobState to) {
    if (!CanTransition(state_, to)) return false;
    state_ = to;
    return true;
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

void Job::RequestCancel() {
    bool transitionedHere = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (IsTerminalState(state_)) return;  // already done, nothing to do
        cancellationRequested_ = true;
        if (state_ == JobState::Queued) {
            // No worker thread will ever run Execute() for a still-Queued job, so
            // nothing else will ever notice the flag -- finalize the transition here.
            transitionedHere = TransitionLocked(JobState::Cancelled);
            if (transitionedHere) completedAt_ = clock_.NowIso8601Utc();
        }
    }
    pauseCv_.notify_all();  // wake WaitWhilePaused() if a paused job is being cancelled
    OnCancel();
    if (transitionedHere) FireStateChanged(JobState::Cancelled);
}

void Job::RequestPause() {
    if (!SupportsPause()) return;
    bool transitioned = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == JobState::Running) transitioned = TransitionLocked(JobState::Paused);
    }
    if (transitioned) {
        OnPause();
        FireStateChanged(JobState::Paused);
    }
}

void Job::RequestResume() {
    bool transitioned = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == JobState::Paused) transitioned = TransitionLocked(JobState::Running);
    }
    if (transitioned) {
        pauseCv_.notify_all();
        OnResume();
        FireStateChanged(JobState::Running);
    }
}

void Job::MarkStarting() {
    JobState previous;
    bool transitioned;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous = state_;
        transitioned = TransitionLocked(JobState::Starting);
    }
    if (!transitioned) ThrowInvalidTransition(previous, JobState::Starting);
    FireStateChanged(JobState::Starting);
}

void Job::MarkRunning() {
    JobState previous;
    bool transitioned;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous = state_;
        transitioned = TransitionLocked(JobState::Running);
        if (transitioned && !startedAt_) startedAt_ = clock_.NowIso8601Utc();
    }
    if (!transitioned) ThrowInvalidTransition(previous, JobState::Running);
    FireStateChanged(JobState::Running);
}

void Job::MarkCompleted() {
    JobState previous;
    bool transitioned;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous = state_;
        transitioned = TransitionLocked(JobState::Completed);
        if (transitioned) completedAt_ = clock_.NowIso8601Utc();
    }
    if (!transitioned) ThrowInvalidTransition(previous, JobState::Completed);
    FireStateChanged(JobState::Completed);
}

void Job::MarkFailed(errors::ErrorInfo error) {
    JobState previous;
    bool transitioned;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous = state_;
        transitioned = TransitionLocked(JobState::Failed);
        if (transitioned) {
            error_ = std::move(error);
            completedAt_ = clock_.NowIso8601Utc();
        }
    }
    if (!transitioned) ThrowInvalidTransition(previous, JobState::Failed);
    FireStateChanged(JobState::Failed);
}

void Job::MarkCancelled() {
    JobState previous;
    bool transitioned;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous = state_;
        transitioned = TransitionLocked(JobState::Cancelled);
        if (transitioned) completedAt_ = clock_.NowIso8601Utc();
    }
    if (!transitioned) ThrowInvalidTransition(previous, JobState::Cancelled);
    FireStateChanged(JobState::Cancelled);
}

void Job::MarkRetrying() {
    JobState previous;
    bool transitioned;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous = state_;
        transitioned = TransitionLocked(JobState::Retrying);
        if (transitioned) {
            cancellationRequested_ = false;
            completedAt_.reset();
        }
    }
    if (!transitioned) ThrowInvalidTransition(previous, JobState::Retrying);
    FireStateChanged(JobState::Retrying);
}

}  // namespace mediatool::jobs
