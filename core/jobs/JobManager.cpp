#include "core/jobs/JobManager.h"

#include "core/errors/MediaToolException.h"
#include "core/logging/Logger.h"

namespace mediatool::jobs {

JobManager::JobManager(std::size_t maxConcurrentJobs)
    : maxConcurrentJobs_(maxConcurrentJobs == 0 ? 1 : maxConcurrentJobs) {
    workers_.reserve(maxConcurrentJobs_);
    for (std::size_t i = 0; i < maxConcurrentJobs_; ++i) {
        workers_.emplace_back([this] { WorkerLoop(); });
    }
}

JobManager::~JobManager() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    queueCv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

JobId JobManager::SubmitJob(std::unique_ptr<Job> job) {
    const JobId id = job->Id();
    job->SetCallbacks(
        [this, id](JobState state) { HandleJobStateChanged(id, state); },
        [this, id](const Progress& progress) { HandleJobProgress(id, progress); });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.emplace(id, std::move(job));
        queue_.push_back(id);
    }
    queueCv_.notify_one();
    return id;
}

Job* JobManager::LookupJobLocked(const JobId& id) const {
    auto it = jobs_.find(id);
    return it == jobs_.end() ? nullptr : it->second.get();
}

void JobManager::ThrowNotFound(const JobId& id) const {
    throw errors::MediaToolException(errors::ErrorInfo::Make(
        "E_JOB_NOT_FOUND", errors::ErrorCategory::Unknown, "Job not found",
        "No job registered with id " + id));
}

void JobManager::ThrowInvalidOperation(const JobId& id, const std::string& reason) const {
    throw errors::MediaToolException(errors::ErrorInfo::Make(
        "E_JOB_INVALID_OPERATION", errors::ErrorCategory::Unknown, reason,
        "Job " + id + ": " + reason));
}

JobManager::JobSnapshot JobManager::SnapshotOf(const Job& job) const {
    JobSnapshot snapshot;
    snapshot.id = job.Id();
    snapshot.type = job.Type();
    snapshot.state = job.State();
    snapshot.progress = job.GetProgress();
    snapshot.error = job.GetError();
    snapshot.result = job.GetResult();
    snapshot.metadata = job.GetMetadata();
    snapshot.createdAt = job.CreatedAt();
    snapshot.startedAt = job.StartedAt();
    snapshot.completedAt = job.CompletedAt();
    return snapshot;
}

nlohmann::json JobManager::JobSnapshot::ToJson() const {
    nlohmann::json json;
    json["id"] = id;
    json["type"] = ToWireString(type);
    json["state"] = ToWireString(state);
    json["createdAt"] = createdAt;
    if (startedAt) json["startedAt"] = *startedAt;
    if (completedAt) json["completedAt"] = *completedAt;
    json["progress"] = progress.ToJson();
    if (error) json["error"] = error->ToJson();
    if (result) json["result"] = *result;
    if (!metadata.is_null() && !(metadata.is_object() && metadata.empty()))
        json["metadata"] = metadata;
    return json;
}

JobManager::JobSnapshot JobManager::GetJob(const JobId& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Job* job = LookupJobLocked(id);
    if (!job) ThrowNotFound(id);
    return SnapshotOf(*job);
}

std::vector<JobManager::JobSnapshot> JobManager::ListJobs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<JobSnapshot> result;
    result.reserve(jobs_.size());
    for (const auto& [id, job] : jobs_) result.push_back(SnapshotOf(*job));
    return result;
}

void JobManager::CancelJob(const JobId& id) {
    Job* job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job = LookupJobLocked(id);
        if (!job) ThrowNotFound(id);
    }
    job->RequestCancel();
}

void JobManager::PauseJob(const JobId& id) {
    Job* job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job = LookupJobLocked(id);
        if (!job) ThrowNotFound(id);
    }
    if (!job->SupportsPause())
        ThrowInvalidOperation(id, "This job type does not support pausing");
    if (job->State() != JobState::Running)
        ThrowInvalidOperation(id, "Job must be Running to be paused");
    job->RequestPause();
}

void JobManager::ResumeJob(const JobId& id) {
    Job* job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job = LookupJobLocked(id);
        if (!job) ThrowNotFound(id);
    }
    if (job->State() != JobState::Paused)
        ThrowInvalidOperation(id, "Job must be Paused to be resumed");
    job->RequestResume();
}

void JobManager::RetryJob(const JobId& id) {
    Job* job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job = LookupJobLocked(id);
        if (!job) ThrowNotFound(id);
    }
    if (job->State() != JobState::Failed)
        ThrowInvalidOperation(id, "Only a Failed job can be retried");
    // MarkRetrying() fires the state-changed callback, which re-enters JobManager (see
    // HandleJobStateChanged) and must not be called while mutex_ is held.
    job->MarkRetrying();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(id);
    }
    queueCv_.notify_one();
}

void JobManager::RemoveJob(const JobId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Job* job = LookupJobLocked(id);
    if (!job) ThrowNotFound(id);
    if (!IsTerminalState(job->State()))
        ThrowInvalidOperation(id, "Only a terminal job (Completed/Failed/Cancelled) can be removed");
    jobs_.erase(id);
}

void JobManager::OnJobStateChanged(JobStateChangedCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    stateChangedCallback_ = std::move(callback);
}

void JobManager::OnJobProgress(JobProgressCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    progressCallback_ = std::move(callback);
}

void JobManager::SetPreMarkStartingHookForTesting(std::function<void(const JobId&)> hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    preMarkStartingHookForTesting_ = std::move(hook);
}

void JobManager::HandleJobStateChanged(const JobId& id, JobState state) {
    JobStateChangedCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = stateChangedCallback_;
    }
    if (callback) callback(id, state);
}

void JobManager::HandleJobProgress(const JobId& id, const Progress& progress) {
    JobProgressCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = progressCallback_;
    }
    if (callback) callback(id, progress);
}

void JobManager::WorkerLoop() {
    while (true) {
        JobId id;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            queueCv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) return;
            id = queue_.front();
            queue_.pop_front();
        }
        // Belt-and-suspenders: RunJob() is expected to convert every exception it can
        // anticipate into a job state transition (Failed/Cancelled) rather than letting
        // it escape. This catch-all exists so that if it ever doesn't -- a bug in RunJob
        // itself, or a genuinely unanticipated exception type -- this worker thread logs
        // and keeps pulling jobs instead of calling std::terminate and taking the whole
        // process down (the exact failure mode of the fixed cancel/start race, #4).
        try {
            RunJob(id);
        } catch (const std::exception& e) {
            logging::Log::Error("JobManager",
                                 "Unhandled exception escaped RunJob for job " + id + ": " + e.what());
        } catch (...) {
            logging::Log::Error("JobManager", "Unhandled non-exception value escaped RunJob for job " + id);
        }
    }
}

void JobManager::RunJob(const JobId& id) {
    Job* job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job = LookupJobLocked(id);
    }
    if (!job) return;  // removed before a worker got to it

    try {
        const JobState pickedUpState = job->State();
        if (pickedUpState == JobState::Queued) {
            std::function<void(const JobId&)> hook;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                hook = preMarkStartingHookForTesting_;
            }
            if (hook) hook(id);
            job->MarkStarting();
            job->MarkRunning();
        } else if (pickedUpState == JobState::Retrying) {
            job->MarkRunning();
        } else {
            // Cancelled (or otherwise moved on) while it was sitting in the queue.
            return;
        }

        job->Execute();
        if (job->State() != JobState::Cancelled) job->MarkCompleted();
    } catch (const errors::MediaToolException& e) {
        if (job->State() == JobState::Cancelled) return;
        if (e.Info().code == "E_INVALID_JOB_TRANSITION") {
            // Benign race (#4): RequestCancel() moved the job to a terminal state (e.g.
            // straight from Queued to Cancelled) between the State() read above and the
            // MarkStarting()/MarkRunning() call that observed it, so that call threw.
            // This is losing a race, not a bug -- treat it the same as any other
            // already-terminal outcome instead of letting it escape and kill the worker.
            if (IsTerminalState(job->State())) return;
            throw;  // not the expected race shape: a genuine state-machine bug, surface it
        }
        if (e.Info().category == errors::ErrorCategory::Cancelled) {
            job->MarkCancelled();
        } else {
            job->MarkFailed(e.Info());
        }
    } catch (const std::exception& e) {
        if (job->State() != JobState::Cancelled) {
            job->MarkFailed(errors::ErrorInfo::Make(
                "E_JOB_UNHANDLED_EXCEPTION", errors::ErrorCategory::Unknown,
                "Job failed unexpectedly", e.what()));
        }
    }
}

}  // namespace mediatool::jobs
