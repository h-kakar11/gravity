#include "core/jobs/JobManager.h"

#include <chrono>
#include <system_error>
#include <vector>

#include "core/errors/MediaToolException.h"
#include "core/logging/Logger.h"

namespace mediatool::jobs {

JobManager::JobManager(std::size_t requestedConcurrentJobs, RetryPolicy retryPolicy,
                        JobWatchdogPolicy watchdogPolicy)
    : retryPolicy_(retryPolicy), watchdogPolicy_(watchdogPolicy) {
    const std::size_t requested = requestedConcurrentJobs == 0 ? 1 : requestedConcurrentJobs;
    workers_.reserve(requested);
    for (std::size_t i = 0; i < requested; ++i) {
        try {
            workers_.emplace_back([this] { WorkerLoop(); });
        } catch (const std::system_error& e) {
            // The OS refused to create a thread (EAGAIN: process/system thread limit, or
            // address space for another stack). Two things must not happen here. First,
            // letting this escape would destroy a half-built workers_ vector whose live
            // threads are still joinable, and destroying a joinable std::thread calls
            // std::terminate -- the same "one bad settings value aborts the process"
            // failure mode as #5, just one layer down. Second, refusing to start at all
            // would let an ambitious concurrentJobs value make the app unusable when a
            // smaller pool would have run every job perfectly well, only slower.
            // So: keep the workers that did start, and carry on with a smaller pool.
            logging::Log::Warning("JobManager",
                                   "Could only start " + std::to_string(workers_.size()) + " of " +
                                       std::to_string(requested) +
                                       " job worker threads; continuing with the smaller pool (" +
                                       e.what() + ")");
            break;
        }
    }

    if (workers_.empty()) {
        // Not a single worker: nothing would ever run, and silently accepting jobs that
        // can never start would be worse than failing loudly here. Nothing to unwind --
        // workers_ is empty, so no joinable thread is destroyed by this throw.
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_WORKER_POOL_UNAVAILABLE", errors::ErrorCategory::EngineFailure,
            "Could not start any background job workers.",
            "std::thread creation failed for all " + std::to_string(requested) + " requested workers"));
    }

    // Reflects the pool that actually exists, not the one that was asked for: every
    // concurrency decision downstream (and MaxConcurrentJobs() itself) must describe
    // reality.
    maxConcurrentJobs_ = workers_.size();

    // Started last, and only when it has something to do. A pool that could not start a
    // single worker never gets here (the throw above), and a zero limit means the caller
    // asked for no watchdog at all.
    if (watchdogPolicy_.maxJobDuration > std::chrono::steady_clock::duration::zero()) {
        try {
            watchdog_ = std::thread([this] { WatchdogLoop(); });
        } catch (const std::system_error& e) {
            // Same reasoning as a short worker pool: running without the backstop is far
            // better than refusing to run at all, but it must be said out loud.
            logging::Log::Warning("JobManager",
                                   std::string("Could not start the stuck-job watchdog; jobs "
                                               "will not be time-limited (") +
                                       e.what() + ")");
        }
    }
}

JobManager::~JobManager() { Shutdown(); }

void JobManager::Shutdown() {
    std::vector<Job*> toCancel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;  // already shut down
        stopping_ = true;

        // Still-pending jobs: cancel and drop from the schedule now, so no worker ever
        // picks them up and starts fresh work after shutdown has begun.
        for (const auto& id : scheduler_.TakeAllPending()) {
            Job* job = LookupJobLocked(id);
            if (job) toCancel.push_back(job);
        }

        // Currently-Running jobs: request cancellation so a well-behaved Execute() that
        // polls IsCancellationRequested() exits promptly rather than running to natural
        // completion while shutdown waits on it.
        for (auto& [id, job] : jobs_) {
            if (job->State() == JobState::Running) toCancel.push_back(job.get());
        }
    }

    // RequestCancel() is safe to call on any job in any state (a no-op once terminal),
    // so calling it here outside the lock -- after a job may have already moved on --
    // is never wrong, just occasionally redundant.
    for (Job* job : toCancel) job->RequestCancel();

    queueCv_.notify_all();
    watchdogCv_.notify_all();

    // Joined before the workers: it wakes on `stopping_` rather than waiting out its
    // interval, and leaving it running while the pool tears down would let it cancel jobs
    // that are already being finalized.
    if (watchdog_.joinable()) watchdog_.join();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

JobId JobManager::SubmitJob(std::unique_ptr<Job> job) {
    const JobId id = job->Id();
    SchedulerCore::Submission submission;
    submission.id = id;
    submission.priority = job->Priority();
    submission.dependsOn = job->DependsOn();
    submission.runAfter = job->RunAfter();

    job->SetCallbacks(
        [this, id](JobState state) { HandleJobStateChanged(id, state); },
        [this, id](const Progress& progress) { HandleJobProgress(id, progress); });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Scheduled first, registered second: if the scheduler rejects the submission (an
        // impossible dependency) it throws from here, and a job the scheduler does not know
        // about must not be sitting in jobs_ where getJob would report it as forever
        // QUEUED. `job` is destroyed as the exception propagates, which is the correct
        // outcome -- it was never accepted.
        scheduler_.Submit(std::move(submission));
        jobs_.emplace(id, std::move(job));
    }
    queueCv_.notify_one();
    return id;
}

Job* JobManager::LookupJobLocked(const JobId& id) const {
    auto it = jobs_.find(id);
    return it == jobs_.end() ? nullptr : it->second.get();
}

void JobManager::CancelStrandedDependents(const std::vector<JobId>& ids, const JobId& because) {
    for (const JobId& id : ids) {
        Job* job = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job = LookupJobLocked(id);
        }
        if (!job) continue;
        logging::Log::Info("JobManager", "Cancelling job " + id + ": it depends on " + because +
                                              ", which did not complete");
        // Outside the lock: this fires the job's state-changed callback, which re-enters
        // HandleJobStateChanged and takes mutex_ -- and, for a chain of dependents, does
        // so recursively one link at a time.
        job->RequestCancel();
    }
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
    snapshot.priority = job.Priority();
    snapshot.attempts = job.AttemptCount();
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
    json["priority"] = priority;
    json["attempts"] = attempts;
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
    // Same reasoning as RetryJob: the transition is the check.
    if (job->RequestPause() != TransitionResult::Success)
        ThrowInvalidOperation(id, "Job must be Running to be paused");
}

void JobManager::ResumeJob(const JobId& id) {
    Job* job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job = LookupJobLocked(id);
        if (!job) ThrowNotFound(id);
    }
    if (job->RequestResume() != TransitionResult::Success)
        ThrowInvalidOperation(id, "Job must be Paused to be resumed");
}

void JobManager::RetryJob(const JobId& id) {
    Job* job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job = LookupJobLocked(id);
        if (!job) ThrowNotFound(id);
    }
    const int priority = job->Priority();
    // No State() == Failed pre-check: reading the state and then transitioning on the
    // strength of that read is the same check-then-act shape that produced #4. The
    // transition itself is the check -- MarkRetrying() only succeeds from Failed, and
    // reports rather than throws when it doesn't, so a job that finished, was removed, or
    // was retried by another caller in the meantime yields an honest error instead of a
    // race. MarkRetrying() fires the state-changed callback, which re-enters JobManager
    // (see HandleJobStateChanged) and must not be called while mutex_ is held.
    if (job->MarkRetrying() != TransitionResult::Success)
        ThrowInvalidOperation(id, "Only a Failed job can be retried");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scheduler_.Requeue(id, priority);
    }
    queueCv_.notify_one();
}

void JobManager::RemoveJob(const JobId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Job* job = LookupJobLocked(id);
    if (!job) ThrowNotFound(id);
    if (!IsTerminalState(job->State()))
        ThrowInvalidOperation(id, "Only a terminal job (Completed/Failed/Cancelled) can be removed");
    // Throws E_JOB_HAS_DEPENDENTS if something queued is still waiting on this job, in
    // which case nothing is removed -- see SchedulerCore::Forget.
    scheduler_.Forget(id);
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
    std::vector<JobId> stranded;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = stateChangedCallback_;
        if (IsTerminalState(state)) {
            // Tell the scheduler how this job ended, both to unblock anything waiting on it
            // and to learn which pending jobs can now never run.
            stranded = scheduler_.RecordTerminal(id, state);
        }
    }

    // A terminal job may have made a dependent eligible, and workers are asleep on a
    // predicate that only the scheduler can answer -- wake them to re-ask.
    if (IsTerminalState(state)) queueCv_.notify_all();

    if (callback) callback(id, state);
    // After the subscriber has seen this job's own outcome, so an observer sees cause
    // before consequence.
    if (!stranded.empty()) CancelStrandedDependents(stranded, id);
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
            // Not "is anything queued" but "is anything *runnable* right now". A queue
            // full of jobs waiting on a dependency must leave workers asleep rather than
            // spinning on entries they cannot start -- and a job waiting out a retry
            // backoff must wake the pool when its time comes rather than only when some
            // unrelated event happens to notify it, which is what the timed wait below is
            // for. Written as a loop rather than a wait predicate because the deadline is
            // recomputed on every pass: a shorter backoff scheduled while this worker
            // slept must shorten the sleep.
            while (!stopping_ && !scheduler_.HasEligible(std::chrono::steady_clock::now())) {
                const std::optional<SchedulerCore::TimePoint> wakeAt =
                    scheduler_.NextEligibleTime(std::chrono::steady_clock::now());
                if (wakeAt) {
                    queueCv_.wait_until(lock, *wakeAt);
                } else {
                    queueCv_.wait(lock);
                }
            }
            if (stopping_) return;
            const std::optional<JobId> next =
                scheduler_.TakeNextEligible(std::chrono::steady_clock::now());
            if (!next) continue;  // lost the race to another worker
            id = *next;
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

    // Claiming the job is a transition attempt, not a state read followed by a
    // transition: whatever State() says a nanosecond ago, only the Mark* call that
    // actually moves the job is authoritative, and it reports losing the race rather than
    // throwing across the worker-thread boundary (#4). Anything other than Success here
    // means someone else finalized this job first -- overwhelmingly a cancellation -- and
    // there is nothing left for this worker to run.
    const JobState pickedUpState = job->State();
    if (pickedUpState == JobState::Retrying) {
        // A retried job is already past Starting; RETRYING -> RUNNING is its only path.
        if (!ClaimedForExecution(*job, job->MarkRunning())) return;
    } else {
        if (!ClaimedForExecution(*job, RunPreMarkStartingHook(id), JobState::Starting)) return;
        if (!ClaimedForExecution(*job, job->MarkRunning())) return;
    }

    try {
        job->Execute();
        // Execute() returning normally after a cancellation was requested is a job type
        // not honoring the cancellation convention (see Job.h) -- finalize as Cancelled
        // rather than reporting work that was cut short as successful.
        if (job->IsCancellationRequested()) {
            job->MarkCancelled();
        } else {
            job->MarkCompleted();
        }
    } catch (const errors::MediaToolException& e) {
        if (e.Info().category == errors::ErrorCategory::Cancelled) {
            job->MarkCancelled();
        } else {
            FinalizeFailure(*job, e.Info());
        }
    } catch (const std::exception& e) {
        FinalizeFailure(*job, errors::ErrorInfo::Make(
                                   "E_JOB_UNHANDLED_EXCEPTION", errors::ErrorCategory::Unknown,
                                   "Job failed unexpectedly", e.what()));
    }
    // Every path above ends in a Mark* whose result is deliberately unexamined: by then
    // the only way it can fail is that a concurrent cancellation already finalized the
    // job, which is precisely the outcome those calls were trying to record.
}

void JobManager::WatchdogLoop() {
    // What this can and cannot do, stated plainly because the difference matters. It
    // requests cancellation, which is genuinely effective: DownloadJob and
    // MediaProcessingJob poll IsCancellationRequested() and both engines terminate (then
    // kill) their child process on seeing it, so a wedged ffmpeg or yt-dlp really does
    // die. What it CANNOT do is force a job whose Execute() ignores cancellation to
    // return -- that thread belongs to the job, and there is no portable way to take it
    // back without killing the process. So after the grace period it reports the job as
    // unresponsive and leaves it alone, rather than marking it Failed while its worker is
    // still running, which would lie about a slot that is not actually free.
    for (;;) {
        std::vector<Job*> overdue;
        std::vector<JobId> unresponsive;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            watchdogCv_.wait_for(lock, watchdogPolicy_.checkInterval, [this] { return stopping_; });
            if (stopping_) return;

            const auto now = std::chrono::steady_clock::now();
            for (const auto& [id, job] : jobs_) {
                const auto runningFor = job->RunningFor();
                if (!runningFor.has_value()) {
                    watchdogCancelledAt_.erase(id);  // not running: nothing outstanding
                    continue;
                }
                const auto threshold = (job->Type() == JobType::Download &&
                                         watchdogPolicy_.downloadMaxJobDuration.has_value())
                                            ? *watchdogPolicy_.downloadMaxJobDuration
                                            : watchdogPolicy_.maxJobDuration;
                if (*runningFor < threshold) continue;

                const auto cancelled = watchdogCancelledAt_.find(id);
                if (cancelled == watchdogCancelledAt_.end()) {
                    watchdogCancelledAt_.emplace(id, now);
                    overdue.push_back(job.get());
                } else if (now - cancelled->second >= watchdogPolicy_.cancellationGrace) {
                    unresponsive.push_back(id);
                    cancelled->second = now;  // re-arm, so this is reported periodically
                }
            }
        }

        // Outside the lock: RequestCancel fires state-changed callbacks that re-enter here.
        for (Job* job : overdue) {
            const auto threshold = (job->Type() == JobType::Download &&
                                     watchdogPolicy_.downloadMaxJobDuration.has_value())
                                        ? *watchdogPolicy_.downloadMaxJobDuration
                                        : watchdogPolicy_.maxJobDuration;
            logging::Log::Warning(
                "JobManager", "Job " + job->Id() + " has been running past the " +
                                   std::to_string(std::chrono::duration_cast<std::chrono::minutes>(threshold)
                                                      .count()) +
                                   "m limit; cancelling it.");
            job->RequestCancel();
        }
        for (const JobId& id : unresponsive) {
            logging::Log::Error("JobManager",
                                 "Job " + id +
                                     " did not stop after cancellation was requested. It is "
                                     "holding a worker slot and cannot be reclaimed without "
                                     "restarting the core.");
        }
    }
}

void JobManager::FinalizeFailure(Job& job, const errors::ErrorInfo& error) {
    const RetryDecision decision = DecideRetry(error, job.AttemptCount(), retryPolicy_);
    if (!decision.shouldRetry) {
        logging::Log::Debug("JobManager", "Job " + job.Id() + " failed for good: " + decision.reason);
        job.MarkFailed(error);
        return;
    }

    // Checked before the transition, not after: a job resurrected during shutdown would
    // be requeued into a scheduler whose pending set has already been drained and
    // cancelled, leaving it parked in RETRYING with no worker left to pick it up. The
    // window between this read and the Requeue below is real but harmless -- the worst
    // case is exactly that parked job, in a process that is exiting anyway.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            job.MarkFailed(error);
            return;
        }
    }

    // RUNNING -> RETRYING directly. Not through FAILED: that would tell the scheduler
    // this job had ended, cancelling everything that depends on it, and write a failure
    // into Session History -- both for an attempt the next line is about to repeat. Fires
    // the state-changed callback, which re-enters this class, so it must not be called
    // under mutex_.
    if (job.MarkRetryScheduled(error) != TransitionResult::Success) {
        return;  // cancelled or otherwise finalized underneath us; nothing to retry
    }

    logging::Log::Info("JobManager", "Retrying job " + job.Id() + " in " +
                                          std::to_string(decision.delay.count()) + "ms: " +
                                          decision.reason);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scheduler_.Requeue(job.Id(), job.Priority(),
                            std::chrono::steady_clock::now() + decision.delay);
    }
    // notify_all, not notify_one: the woken worker may not be the one whose timed wait is
    // shortest, and a worker asleep on a later deadline needs to recompute it.
    queueCv_.notify_all();
}

// Runs the testing-only interleaving hook (if any) and then attempts the Queued ->
// Starting claim. Split out so RunJob reads as the three transition attempts it is.
TransitionResult JobManager::RunPreMarkStartingHook(const JobId& id) {
    std::function<void(const JobId&)> hook;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hook = preMarkStartingHookForTesting_;
    }
    if (hook) hook(id);
    Job* job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job = LookupJobLocked(id);
    }
    return job ? job->MarkStarting() : TransitionResult::AlreadyTerminal;
}

bool JobManager::ClaimedForExecution(const Job& job, TransitionResult result, JobState attempted) {
    if (result == TransitionResult::Success) return true;
    if (result == TransitionResult::InvalidTransition) {
        // Not a race: the job was in a non-terminal state this transition is illegal from,
        // which means the scheduler and the state machine disagree about the lifecycle.
        // Log it -- silently dropping the job would hide a real bug -- but still decline
        // to run it, because forcing the transition is how you corrupt a state machine.
        logging::Log::Error("JobManager", "Refusing to run job " + job.Id() + ": cannot enter " +
                                               ToWireString(attempted) + " from " +
                                               ToWireString(job.State()));
    }
    return false;
}

}  // namespace mediatool::jobs
