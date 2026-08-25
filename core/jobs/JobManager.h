#pragma once

// Job orchestration (spec section 6). Owns every submitted Job, runs them on a small
// fixed worker-thread pool sized by `maxConcurrentJobs` (constructor parameter, NOT
// hardcoded -- spec section 6 requires this to scale to N later even though Phase 1
// callers may pass 1), and exposes subscription points
// (OnJobStateChanged/OnJobProgress) that a later integration pass bridges into the
// EventBus this module does not own or know about.
//
// Concurrency model (deliberately simple per spec section 41): one queue of pending
// JobIds, N worker threads each pulling one JobId at a time and running it to
// completion before pulling the next. That alone caps concurrent execution at N without
// needing a separate "running" counter.

#include <condition_variable>
#include <deque>
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
#include "core/jobs/JobTypes.h"
#include "core/jobs/Progress.h"

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

    explicit JobManager(std::size_t maxConcurrentJobs = 1);
    ~JobManager();

    JobManager(const JobManager&) = delete;
    JobManager& operator=(const JobManager&) = delete;

    std::size_t MaxConcurrentJobs() const { return maxConcurrentJobs_; }

    // Registers `job` and enqueues it for execution on the worker pool. Ownership of
    // `job` passes to the JobManager. Returns its JobId (job->Id()).
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
    // Only valid from Failed.
    void RetryJob(const JobId& id);

    // Drops a job in a terminal state (Completed/Failed/Cancelled) from the active set.
    // Throws if `id` is unknown or the job is not yet terminal.
    void RemoveJob(const JobId& id);

    // Replaces any previously-registered subscriber (not additive). Called from
    // whichever thread is running the job at the time of the change.
    void OnJobStateChanged(JobStateChangedCallback callback);
    void OnJobProgress(JobProgressCallback callback);

private:
    void WorkerLoop();
    void RunJob(const JobId& id);
    Job* LookupJobLocked(const JobId& id) const;
    JobSnapshot SnapshotOf(const Job& job) const;
    void HandleJobStateChanged(const JobId& id, JobState state);
    void HandleJobProgress(const JobId& id, const Progress& progress);
    [[noreturn]] void ThrowNotFound(const JobId& id) const;
    [[noreturn]] void ThrowInvalidOperation(const JobId& id, const std::string& reason) const;

    const std::size_t maxConcurrentJobs_;

    mutable std::mutex mutex_;
    std::map<JobId, std::unique_ptr<Job>> jobs_;
    std::deque<JobId> queue_;
    std::condition_variable queueCv_;
    bool stopping_ = false;

    JobStateChangedCallback stateChangedCallback_;
    JobProgressCallback progressCallback_;

    std::vector<std::thread> workers_;
};

}  // namespace mediatool::jobs
