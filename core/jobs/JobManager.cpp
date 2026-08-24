#include "core/jobs/JobManager.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "core/errors/MediaToolException.h"
#include "core/logging/Logger.h"
#include "core/queue/BackoffPolicy.h"
#include "core/queue/RetryClassifier.h"

namespace mediatool::jobs {

namespace {

constexpr const char* kLogSubsystem = "JobManager";

// Upper bound on how long the scheduler sleeps with nothing time-gated pending. It is not
// a polling interval -- every state change wakes the scheduler explicitly -- just a
// backstop so a missed notification degrades into a small delay rather than a wedged queue.
constexpr std::int64_t kSchedulerIdleTimeoutMs = 1'000;

}  // namespace

// --- construction -------------------------------------------------------------------------

JobManager::JobManager(std::size_t maxConcurrentJobs)
    : JobManager(
          [maxConcurrentJobs] {
              Options options;
              options.maxConcurrentJobs = maxConcurrentJobs == 0 ? 1 : maxConcurrentJobs;
              return options;
          }(),
          nullptr) {}

JobManager::JobManager(Options options, IJobFactory* factory)
    : options_(std::move(options)),
      factory_(factory),
      ownedClock_(std::make_unique<common::SystemClock>()),
      clock_(*ownedClock_) {
    queue::SchedulerCore::Config config;
    config.maxConcurrency = options_.maxConcurrentJobs == 0 ? 1 : options_.maxConcurrentJobs;
    config.agingIntervalMs = options_.agingIntervalMs;
    config.historyLimit = options_.historyLimit;
    scheduler_ = queue::SchedulerCore(config);
    if (!options_.stateFilePath.empty())
        persistence_ = std::make_unique<queue::QueuePersistence>(options_.stateFilePath);
    Start();
}

JobManager::JobManager(Options options, IJobFactory* factory, common::IClock& clock)
    : options_(std::move(options)), factory_(factory), ownedClock_(nullptr), clock_(clock) {
    queue::SchedulerCore::Config config;
    config.maxConcurrency = options_.maxConcurrentJobs == 0 ? 1 : options_.maxConcurrentJobs;
    config.agingIntervalMs = options_.agingIntervalMs;
    config.historyLimit = options_.historyLimit;
    scheduler_ = queue::SchedulerCore(config);
    if (!options_.stateFilePath.empty())
        persistence_ = std::make_unique<queue::QueuePersistence>(options_.stateFilePath);
    // Deliberately does NOT Start(): the clock-injecting constructor is the one tests use,
    // and they drive scheduling explicitly via RunSchedulerPassForTesting() so that no
    // background thread races their assertions.
}

JobManager::~JobManager() {
    Shutdown();
}

void JobManager::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_ || stopping_) return;
    started_ = true;
    schedulerThread_ = std::thread([this] { SchedulerLoop(); });
    logging::Log::Info(kLogSubsystem, "scheduler started");
}

void JobManager::Shutdown() {
    std::vector<std::thread> toJoin;
    std::thread scheduler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
        scheduler = std::move(schedulerThread_);
        for (auto& [id, worker] : workers_) toJoin.push_back(std::move(worker));
        workers_.clear();
    }
    schedulerCv_.notify_all();

    // Joined outside the lock: a worker finishing right now is very likely blocked trying
    // to enter HandleJobStateChanged, and joining it while holding mutex_ would deadlock.
    if (scheduler.joinable()) scheduler.join();
    for (auto& worker : toJoin) {
        if (worker.joinable()) worker.join();
    }

    PersistIfDue(NowMs(), /*force=*/true);
    logging::Log::Info(kLogSubsystem, "scheduler stopped");
}

std::int64_t JobManager::NowMs() const {
    return clock_.NowUnixMillis();
}

std::size_t JobManager::MaxConcurrentJobs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return scheduler_.GetConfig().maxConcurrency;
}

void JobManager::WakeScheduler() {
    schedulerCv_.notify_all();
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

std::shared_ptr<Job> JobManager::LookupLocked(const JobId& id) const {
    const auto it = jobs_.find(id);
    return it == jobs_.end() ? nullptr : it->second;
}

// --- submission ----------------------------------------------------------------------------

JobId JobManager::SubmitJob(std::unique_ptr<Job> job) {
    SubmitRequest request;
    request.spec.type = job->Type();
    request.retryPolicy = options_.defaultRetryPolicy;
    return SubmitJob(std::move(job), request);
}

JobId JobManager::SubmitJob(std::unique_ptr<Job> job, const SubmitRequest& request) {
    if (!job) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_INVALID_JOB", errors::ErrorCategory::Unknown, "A job is required."));
    }

    const JobId id = job->Id();
    std::shared_ptr<Job> shared(std::move(job));

    queue::JobRecord record;
    record.id = id;
    record.spec = request.spec;
    record.priority = request.priority;
    record.dependencies = request.dependencies;
    record.parentJobId = request.parentJobId;
    record.retryPolicy = request.retryPolicy.value_or(options_.defaultRetryPolicy);
    record.retryPolicy.Validate();
    record.duplicateKey = request.allowDuplicate ? std::string() : request.duplicateKey;
    record.metadata = request.metadata;

    JobState initialState;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Insert first: it validates duplicates, unknown dependencies and cycles, and
        // throws without mutating anything. Registering the Job before that would leave an
        // orphan behind on a rejected request.
        scheduler_.Insert(record, NowMs());
        jobs_.emplace(id, shared);
        const queue::JobRecord* inserted = scheduler_.Find(id);
        initialState = inserted != nullptr ? inserted->state : JobState::Queued;
        dirty_ = true;
    }

    // Callbacks are wired after insertion so a state change cannot arrive before the
    // scheduler knows the job exists.
    shared->SetCallbacks([this, id](JobState state) { HandleJobStateChanged(id, state); },
                         [this, id](const Progress& progress) { HandleJobProgress(id, progress); });

    // The scheduler decided this job is dependency-blocked; move the live Job to match.
    // Done outside the lock because MarkWaiting fires a callback that re-enters this class.
    if (initialState == JobState::Waiting) shared->MarkWaiting();

    logging::Log::Info(kLogSubsystem, "job created: " + id + " (" + ToWireString(shared->Type()) +
                                          ", " + ToWireString(initialState) + ")");
    NotifyQueueChanged();
    WakeScheduler();
    return id;
}

// --- scheduler -------------------------------------------------------------------------------

void JobManager::SchedulerLoop() {
    std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);
    while (true) {
        {
            lock.lock();
            if (stopping_) {
                lock.unlock();
                break;
            }
            lock.unlock();
        }

        const std::optional<std::int64_t> nextWake = SchedulerPass();

        lock.lock();
        if (stopping_) {
            lock.unlock();
            break;
        }
        const std::int64_t now = NowMs();
        std::int64_t waitMs = kSchedulerIdleTimeoutMs;
        if (nextWake.has_value()) waitMs = std::max<std::int64_t>(0, *nextWake - now);
        waitMs = std::min(waitMs, kSchedulerIdleTimeoutMs);
        schedulerCv_.wait_for(lock, std::chrono::milliseconds(waitMs),
                              [this] { return stopping_; });
        lock.unlock();
    }

    // Drain: let whatever is still running finish, so Shutdown() can join cleanly.
    while (true) {
        std::vector<std::thread> toJoin;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            for (const auto& id : finishedWorkers_) {
                const auto it = workers_.find(id);
                if (it == workers_.end()) continue;
                toJoin.push_back(std::move(it->second));
                workers_.erase(it);
            }
            finishedWorkers_.clear();
            if (workers_.empty() && toJoin.empty()) break;
        }
        for (auto& worker : toJoin) {
            if (worker.joinable()) worker.join();
        }
        if (toJoin.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

std::optional<std::int64_t> JobManager::SchedulerPass() {
    const std::int64_t now = NowMs();

    std::vector<queue::PendingTransition> transitions;
    std::vector<std::pair<JobId, std::shared_ptr<Job>>> toDispatch;
    std::vector<std::thread> toJoin;
    std::vector<std::pair<JobId, Progress>> progressToFlush;
    std::optional<std::int64_t> nextWake;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // 1. Reap finished worker threads. Their thread objects are moved out here and
        //    joined below, outside the lock.
        for (const auto& id : finishedWorkers_) {
            const auto it = workers_.find(id);
            if (it == workers_.end()) continue;
            toJoin.push_back(std::move(it->second));
            workers_.erase(it);
        }
        finishedWorkers_.clear();

        // 2. Release or skip jobs whose dependencies resolved.
        transitions = scheduler_.ResolveDependencies(now);

        // 3. Admission. SchedulerCore marks each returned job Starting, so nothing here can
        //    hand the same job out twice even if this pass is re-entered.
        for (const auto& id : scheduler_.SelectDispatchable(now)) {
            auto job = LookupLocked(id);
            if (job == nullptr) {
                // The record outlived its Job -- treat it as failed rather than leaking a
                // permanently-Starting record that would consume a slot forever.
                scheduler_.SetState(id, JobState::Failed, now);
                continue;
            }
            toDispatch.emplace_back(id, std::move(job));
        }

        // 4. Flush progress updates that throttling has been holding back, so a long, quiet
        //    job still shows its most recent progress rather than a stale one.
        for (auto& [id, throttle] : progressThrottles_) {
            if (!throttle.pending.has_value()) continue;
            if (now - throttle.lastEmittedMs < options_.progressIntervalMs) continue;
            progressToFlush.emplace_back(id, *throttle.pending);
            throttle.pending.reset();
            throttle.lastEmittedMs = now;
        }

        nextWake = scheduler_.NextWakeupMs(now);
    }

    for (auto& worker : toJoin) {
        if (worker.joinable()) worker.join();
    }

    ApplyDependencyTransitions(transitions);

    // Dispatch outside the lock: starting a worker means calling into Job, which fires
    // callbacks straight back into this class.
    for (auto& [id, job] : toDispatch) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            // Put the job back rather than starting work we are about to abandon.
            scheduler_.SetState(id, JobState::Queued, now);
            continue;
        }
        workers_.emplace(id, std::thread([this, job, id] { RunJob(job, id); }));
    }

    {
        JobProgressCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = progressCallback_;
        }
        if (callback) {
            for (const auto& [id, progress] : progressToFlush) callback(id, progress);
        }
    }

    if (!toDispatch.empty() || !transitions.empty()) NotifyQueueChanged();

    PersistIfDue(now, /*force=*/false);
    return nextWake;
}

void JobManager::RunSchedulerPassForTesting() {
    SchedulerPass();

    // Let anything this pass started run to completion, so a test can assert on the result
    // without sleeping. Bounded so a genuinely stuck job fails the test instead of hanging
    // the suite forever.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        std::vector<std::thread> toJoin;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (workers_.empty()) break;
            for (const auto& id : finishedWorkers_) {
                const auto it = workers_.find(id);
                if (it == workers_.end()) continue;
                toJoin.push_back(std::move(it->second));
                workers_.erase(it);
            }
            finishedWorkers_.clear();
        }
        for (auto& worker : toJoin) {
            if (worker.joinable()) worker.join();
        }
        if (toJoin.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void JobManager::ApplyDependencyTransitions(
    const std::vector<queue::PendingTransition>& transitions) {
    for (const auto& transition : transitions) {
        std::shared_ptr<Job> job;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job = LookupLocked(transition.id);
        }
        if (job == nullptr) continue;
        try {
            switch (transition.newState) {
                case JobState::Queued:
                    if (job->State() == JobState::Waiting) job->MarkQueued();
                    break;
                case JobState::Waiting:
                    if (job->State() == JobState::Queued) job->MarkWaiting();
                    break;
                case JobState::Skipped:
                    if (!IsTerminalState(job->State())) {
                        job->MarkSkipped(transition.reason.value_or(errors::ErrorInfo::Make(
                            "E_DEPENDENCY_FAILED", errors::ErrorCategory::Unknown,
                            "Skipped because a job it depended on did not finish.")));
                    }
                    break;
                default:
                    break;
            }
        } catch (const errors::MediaToolException& e) {
            // The Job raced us into another state (a cancel landing at the same moment).
            // Its state is authoritative -- re-sync the record instead of forcing the issue.
            std::lock_guard<std::mutex> lock(mutex_);
            scheduler_.SetState(transition.id, job->State(), NowMs());
            logging::Log::Debug(kLogSubsystem, "dependency transition superseded for " +
                                                   transition.id + ": " + e.Info().details);
        }
    }
}

void JobManager::RunJob(std::shared_ptr<Job> job, JobId id) {
    // Whatever happens below, this worker must announce that it is done so the scheduler
    // can join it and free the slot. A worker that returned without doing so would leak a
    // thread and wedge the queue.
    struct FinishGuard {
        JobManager* manager;
        JobId id;
        ~FinishGuard() {
            {
                std::lock_guard<std::mutex> lock(manager->mutex_);
                manager->finishedWorkers_.push_back(id);
            }
            manager->WakeScheduler();
        }
    } guard{this, id};

    try {
        const JobState pickedUp = job->State();
        if (pickedUp == JobState::Queued) {
            job->MarkStarting();
            job->MarkRunning();
        } else if (pickedUp == JobState::RetryWait) {
            job->MarkRetrying();
            job->MarkRunning();
        } else if (pickedUp == JobState::Retrying) {
            job->MarkRunning();
        } else {
            // Cancelled (or otherwise moved on) between dispatch and pickup. Re-sync the
            // record so the slot is released rather than held by a job that will never run.
            std::lock_guard<std::mutex> lock(mutex_);
            scheduler_.SetState(id, job->State(), NowMs());
            return;
        }
    } catch (const errors::MediaToolException& e) {
        std::lock_guard<std::mutex> lock(mutex_);
        scheduler_.SetState(id, job->State(), NowMs());
        logging::Log::Warning(kLogSubsystem, "could not start job " + id + ": " + e.Info().message);
        return;
    }

    logging::Log::Info(kLogSubsystem, "job running: " + id);

    try {
        // Inside the try: if the producer's output cannot be resolved, that is this job's
        // failure and belongs in its error, not an exception escaping the worker thread.
        ResolveDependencyInput(job, id);
        job->Execute();
        // Completion and cancellation can land together. Whichever transition the state
        // machine accepts first wins and the other is a no-op, so exactly one terminal
        // result is ever recorded (spec section 39, case 3).
        if (job->State() == JobState::Running) job->MarkCompleted();
    } catch (const errors::MediaToolException& e) {
        if (IsTerminalState(job->State())) return;  // already resolved by a racing cancel
        if (e.Info().category == errors::ErrorCategory::Cancelled) {
            job->MarkCancelled();
        } else {
            job->MarkFailed(e.Info());
        }
    } catch (const std::exception& e) {
        if (!IsTerminalState(job->State())) {
            job->MarkFailed(errors::ErrorInfo::Make("E_JOB_UNHANDLED_EXCEPTION",
                                                     errors::ErrorCategory::Unknown,
                                                     "Job failed unexpectedly", e.what()));
        }
    } catch (...) {
        // A non-std exception must not take the process down and must not leave the slot
        // occupied. One broken job never kills the queue (spec section 42).
        if (!IsTerminalState(job->State())) {
            job->MarkFailed(errors::ErrorInfo::Make("E_JOB_UNHANDLED_EXCEPTION",
                                                     errors::ErrorCategory::Unknown,
                                                     "Job failed unexpectedly",
                                                     "non-standard exception"));
        }
    }
}

void JobManager::ResolveDependencyInput(const std::shared_ptr<Job>& job, const JobId& id) {
    JobId sourceId;
    std::shared_ptr<Job> source;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const queue::JobRecord* record = scheduler_.Find(id);
        if (record == nullptr) return;
        if (!record->spec.params.contains("inputFromJobId")) return;
        const auto& value = record->spec.params.at("inputFromJobId");
        if (!value.is_string()) return;
        sourceId = value.get<std::string>();
        if (sourceId.empty()) return;
        source = LookupLocked(sourceId);
    }

    // The scheduler only dispatches a job once every dependency has COMPLETED, so a
    // producer that is missing or has no result here means something is genuinely wrong --
    // report it rather than running against an empty input path and blaming the file.
    const auto fail = [&sourceId](const std::string& detail) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_DEPENDENCY_OUTPUT_MISSING", errors::ErrorCategory::FileNotFound,
            "The job this one takes its input from did not produce a file.",
            "source job " + sourceId + ": " + detail));
    };

    if (source == nullptr) fail("no longer in the queue");
    const auto result = source->GetResult();
    if (!result || !result->contains("outputPath") || !(*result)["outputPath"].is_string())
        fail("completed without recording an output path");

    const std::string resolved = (*result)["outputPath"].get<std::string>();
    logging::Log::Debug(kLogSubsystem, "job " + id + " takes its input from " + sourceId +
                                           " -> " + resolved);
    job->ApplyResolvedInput(resolved);
}

// --- callbacks from Job --------------------------------------------------------------------

void JobManager::HandleJobStateChanged(const JobId& id, JobState state) {
    JobStateChangedCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scheduler_.SetState(id, state, NowMs());
        dirty_ = true;
        callback = stateChangedCallback_;
        if (IsTerminalState(state)) progressThrottles_.erase(id);
    }

    // Outside the lock: this may call back into Job (MarkRetryWait), and subscribers are
    // free to call back into this class.
    if (state == JobState::Failed) MaybeScheduleAutomaticRetry(id);

    if (callback) callback(id, state);
    NotifyQueueChanged();
    WakeScheduler();
}

void JobManager::HandleJobProgress(const JobId& id, const Progress& progress) {
    JobProgressCallback callback;
    bool emitNow = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::int64_t now = NowMs();
        ProgressThrottle& throttle = progressThrottles_[id];
        if (throttle.lastEmittedMs == 0 || now - throttle.lastEmittedMs >= options_.progressIntervalMs) {
            throttle.lastEmittedMs = now;
            throttle.pending.reset();
            emitNow = true;
        } else {
            // Coalesce: hold the newest value and let the scheduler's next pass emit it.
            // Progress is volatile display data -- dropping intermediate frames is correct,
            // losing the latest one is not (spec section 51).
            throttle.pending = progress;
        }
        callback = progressCallback_;
    }
    // Progress deliberately does NOT mark the queue dirty: persisting on every progress
    // tick is exactly the design spec section 52 forbids.
    if (emitNow && callback) callback(id, progress);
}

void JobManager::MaybeScheduleAutomaticRetry(const JobId& id) {
    std::shared_ptr<Job> job;
    std::optional<errors::ErrorInfo> error;
    int attempt = 0;
    std::int64_t delayMs = 0;
    std::string reason;
    bool schedule = false;
    RetryScheduledCallback callback;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        job = LookupLocked(id);
        const queue::JobRecord* record = scheduler_.Find(id);
        if (job == nullptr || record == nullptr) return;
        if (record->retryPolicy.maxRetries <= 0) return;

        error = job->GetError();
        if (!error.has_value()) return;

        const auto classification = queue::ClassifyRetry(*error);
        if (!classification.IsTransient()) {
            logging::Log::Info(kLogSubsystem, "no retry for " + id + ": " + classification.reason);
            return;
        }
        if (!scheduler_.HasRetryBudget(id)) {
            logging::Log::Info(kLogSubsystem, "retry budget exhausted for " + id + " after " +
                                                  std::to_string(record->attempt) + " attempts");
            return;
        }

        attempt = record->attempt + 1;
        delayMs = queue::BackoffDelayMs(record->retryPolicy, attempt);
        reason = classification.reason;
        schedule = true;
        callback = retryScheduledCallback_;
    }

    if (!schedule) return;

    // Job first, scheduler second: the Job's state machine is the authority on whether
    // FAILED -> RETRY_WAIT is even legal right now (a cancel may have landed in between).
    try {
        job->MarkRetryWait();
    } catch (const errors::MediaToolException&) {
        return;  // no longer Failed -- something else resolved this job
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        scheduler_.ScheduleRetry(id, delayMs, NowMs(), reason);
        dirty_ = true;
    }

    logging::Log::Info(kLogSubsystem, "retry " + std::to_string(attempt) + " scheduled for " + id +
                                          " in " + std::to_string(delayMs) + "ms (" + reason + ")");
    if (callback) callback(id, attempt, delayMs, reason);
    NotifyQueueChanged();
    WakeScheduler();
}

// --- inspection --------------------------------------------------------------------------

JobManager::JobSnapshot JobManager::SnapshotLocked(const JobId& id) const {
    const auto jobIt = jobs_.find(id);
    const queue::JobRecord* record = scheduler_.Find(id);
    if (jobIt == jobs_.end() && record == nullptr) ThrowNotFound(id);

    JobSnapshot snapshot;
    snapshot.id = id;

    if (jobIt != jobs_.end()) {
        const Job& job = *jobIt->second;
        snapshot.type = job.Type();
        snapshot.state = job.State();
        snapshot.progress = job.GetProgress();
        snapshot.error = job.GetError();
        snapshot.result = job.GetResult();
        snapshot.metadata = job.GetMetadata();
        snapshot.createdAt = job.CreatedAt();
        snapshot.startedAt = job.StartedAt();
        snapshot.completedAt = job.CompletedAt();
    } else if (record != nullptr) {
        snapshot.type = record->spec.type;
        snapshot.state = record->state;
        snapshot.metadata = record->metadata;
    }

    if (record != nullptr) {
        snapshot.priority = record->priority;
        snapshot.attempt = record->attempt;
        snapshot.maxRetries = record->retryPolicy.maxRetries;
        snapshot.nextRetryAtMs = record->nextRetryAtMs;
        snapshot.lastRetryReason = record->lastRetryReason;
        snapshot.dependencies = record->dependencies;
        snapshot.parentJobId = record->parentJobId;
        snapshot.revision = record->revision;
        if (!record->metadata.empty()) {
            // The record's descriptive metadata is the copy that survives a restart; the
            // Job's own is richer once it has run. Merge so the UI sees both.
            nlohmann::json merged = record->metadata;
            merged.update(snapshot.metadata);
            snapshot.metadata = merged;
        }

        const auto& order = scheduler_.PendingOrder();
        const auto position = std::find(order.begin(), order.end(), id);
        if (position != order.end())
            snapshot.queuePosition = static_cast<int>(std::distance(order.begin(), position));
    }
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

    json["priority"] = queue::ToWireString(priority);
    json["attempt"] = attempt;
    json["retryCount"] = attempt;  // the spec's name for the same number
    json["maxRetries"] = maxRetries;
    if (nextRetryAtMs) json["nextRetryAtMs"] = *nextRetryAtMs;
    if (!lastRetryReason.empty()) json["retryReason"] = lastRetryReason;
    json["dependencies"] = dependencies;
    if (parentJobId) json["parentJobId"] = *parentJobId;
    if (queuePosition) json["queuePosition"] = *queuePosition;
    json["revision"] = revision;
    return json;
}

nlohmann::json JobManager::QueueSnapshot::ToJson() const {
    nlohmann::json array = nlohmann::json::array();
    for (const auto& job : jobs) array.push_back(job.ToJson());
    return {{"runState", queue::ToWireString(runState)},
            {"maxConcurrency", maxConcurrency},
            {"statistics", statistics.ToJson()},
            {"jobs", array},
            {"pendingOrder", pendingOrder},
            {"sequence", sequence}};
}

JobManager::JobSnapshot JobManager::GetJob(const JobId& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return SnapshotLocked(id);
}

std::vector<JobManager::JobSnapshot> JobManager::ListJobs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<JobSnapshot> result;
    for (const auto& record : scheduler_.Snapshot()) result.push_back(SnapshotLocked(record.id));
    return result;
}

JobManager::QueueSnapshot JobManager::GetQueueSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    QueueSnapshot snapshot;
    snapshot.runState = scheduler_.RunState();
    snapshot.maxConcurrency = scheduler_.GetConfig().maxConcurrency;
    snapshot.statistics = scheduler_.Stats();
    snapshot.pendingOrder = scheduler_.PendingOrder();
    for (const auto& record : scheduler_.Snapshot()) snapshot.jobs.push_back(SnapshotLocked(record.id));
    return snapshot;
}

// --- per-job control ------------------------------------------------------------------------

void JobManager::CancelJob(const JobId& id) {
    std::shared_ptr<Job> job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job = LookupLocked(id);
        if (job == nullptr) ThrowNotFound(id);
    }
    job->RequestCancel();
    WakeScheduler();
}

void JobManager::PauseJob(const JobId& id) {
    std::shared_ptr<Job> job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job = LookupLocked(id);
        if (job == nullptr) ThrowNotFound(id);
    }
    if (!job->SupportsPause()) {
        // Deliberate: there is no honest way to pause a running yt-dlp or ffmpeg process on
        // this platform, so this reports the truth instead of showing "Paused" over a
        // process that is still consuming CPU and bandwidth (spec section 12).
        ThrowInvalidOperation(id, "This job type cannot be paused; cancel it instead");
    }
    if (job->State() != JobState::Running)
        ThrowInvalidOperation(id, "Job must be Running to be paused");
    job->RequestPause();
}

void JobManager::ResumeJob(const JobId& id) {
    std::shared_ptr<Job> job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job = LookupLocked(id);
        if (job == nullptr) ThrowNotFound(id);
    }
    if (job->State() != JobState::Paused)
        ThrowInvalidOperation(id, "Job must be Paused to be resumed");
    job->RequestResume();
}

void JobManager::RetryJob(const JobId& id) {
    std::shared_ptr<Job> job;
    JobState state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job = LookupLocked(id);
        if (job == nullptr) ThrowNotFound(id);
        state = job->State();
    }

    if (state == JobState::Skipped) {
        // A skipped job's dependencies may since have been retried. Send it back through
        // dependency evaluation rather than assuming it can run.
        job->MarkWaiting();
        {
            // Scoped deliberately: NotifyQueueChanged() takes mutex_ itself, and mutex_ is
            // not recursive, so it must be called after this guard has gone out of scope.
            std::lock_guard<std::mutex> lock(mutex_);
            scheduler_.SetState(id, JobState::Waiting, NowMs());
            dirty_ = true;
        }
        NotifyQueueChanged();
        WakeScheduler();
        return;
    }

    if (state != JobState::Failed && state != JobState::RetryWait) {
        ThrowInvalidOperation(id, "Only a failed or retrying job can be retried");
    }

    if (state == JobState::Failed) job->MarkRetryWait();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scheduler_.PrepareManualRetry(id, NowMs());
        dirty_ = true;
    }
    logging::Log::Info(kLogSubsystem, "manual retry requested for " + id);
    NotifyQueueChanged();
    WakeScheduler();
}

void JobManager::SetJobPriority(const JobId& id, queue::JobPriority priority) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (jobs_.find(id) == jobs_.end() && !scheduler_.Contains(id)) ThrowNotFound(id);
        scheduler_.SetPriority(id, priority);
        dirty_ = true;
    }
    NotifyQueueChanged();
    WakeScheduler();
}

void JobManager::MoveJob(const JobId& id, queue::MoveDirection direction) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (jobs_.find(id) == jobs_.end() && !scheduler_.Contains(id)) ThrowNotFound(id);
        scheduler_.Move(id, direction);
        dirty_ = true;
    }
    NotifyQueueChanged();
    WakeScheduler();
}

void JobManager::RemoveJob(const JobId& id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (jobs_.find(id) == jobs_.end() && !scheduler_.Contains(id)) ThrowNotFound(id);
        scheduler_.Remove(id);  // throws unless terminal
        jobs_.erase(id);
        progressThrottles_.erase(id);
        dirty_ = true;
    }
    NotifyQueueChanged();
}

// --- queue control --------------------------------------------------------------------------

void JobManager::PauseQueue() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scheduler_.SetRunState(queue::QueueRunState::Paused);
        dirty_ = true;
    }
    logging::Log::Info(kLogSubsystem, "queue paused (running jobs continue)");
    NotifyQueueChanged();
}

void JobManager::ResumeQueue() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scheduler_.SetRunState(queue::QueueRunState::Running);
        dirty_ = true;
    }
    logging::Log::Info(kLogSubsystem, "queue resumed");
    NotifyQueueChanged();
    WakeScheduler();
}

void JobManager::SetMaxConcurrency(std::size_t value) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scheduler_.SetMaxConcurrency(value);
        dirty_ = true;
    }
    logging::Log::Info(kLogSubsystem, "concurrency set to " + std::to_string(value));
    NotifyQueueChanged();
    WakeScheduler();
}

std::vector<JobId> JobManager::ClearHistory(queue::HistoryScope scope) {
    std::vector<JobId> removed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        removed = scheduler_.ClearHistory(scope);
        for (const auto& id : removed) {
            jobs_.erase(id);
            progressThrottles_.erase(id);
        }
        dirty_ = true;
    }
    logging::Log::Info(kLogSubsystem,
                       "cleared " + std::to_string(removed.size()) + " history entries");
    NotifyQueueChanged();
    return removed;
}

std::vector<JobId> JobManager::RetryAllFailed() {
    std::vector<JobId> candidates;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& record : scheduler_.Snapshot()) {
            if (record.state == JobState::Failed) candidates.push_back(record.id);
        }
    }

    std::vector<JobId> retried;
    for (const auto& id : candidates) {
        try {
            RetryJob(id);
            retried.push_back(id);
        } catch (const errors::MediaToolException&) {
            // Raced into another state between listing and retrying. Skip it; the rest of
            // the batch must still go through.
        }
    }
    return retried;
}

// --- subscriptions ---------------------------------------------------------------------------

void JobManager::OnJobStateChanged(JobStateChangedCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    stateChangedCallback_ = std::move(callback);
}

void JobManager::OnJobProgress(JobProgressCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    progressCallback_ = std::move(callback);
}

void JobManager::OnQueueChanged(QueueChangedCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    queueChangedCallback_ = std::move(callback);
}

void JobManager::OnRetryScheduled(RetryScheduledCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    retryScheduledCallback_ = std::move(callback);
}

void JobManager::NotifyQueueChanged() {
    QueueChangedCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = queueChangedCallback_;
    }
    if (callback) callback();
}

// --- persistence -------------------------------------------------------------------------------

void JobManager::MarkDirty() {
    std::lock_guard<std::mutex> lock(mutex_);
    dirty_ = true;
}

queue::PersistedQueue JobManager::BuildPersistedQueueLocked() const {
    queue::PersistedQueue persisted;
    persisted.schemaVersion = queue::kQueueSchemaVersion;
    persisted.runState = scheduler_.RunState();
    persisted.maxConcurrency = scheduler_.GetConfig().maxConcurrency;
    persisted.pendingOrder = scheduler_.PendingOrder();
    for (auto record : scheduler_.Snapshot()) {
        // Capture whatever the live Job has learned (title, resolved output path, ...) so a
        // restored queue can still describe itself before anything re-runs.
        const auto it = jobs_.find(record.id);
        if (it != jobs_.end()) {
            nlohmann::json merged = record.metadata;
            merged.update(it->second->GetMetadata());
            record.metadata = merged;
        }
        persisted.records.push_back(std::move(record));
    }
    return persisted;
}

void JobManager::PersistIfDue(std::int64_t nowMs, bool force) {
    if (persistence_ == nullptr) return;

    queue::PersistedQueue persisted;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!dirty_ && !force) return;
        if (!force && nowMs - lastPersistMs_ < options_.persistIntervalMs) return;
        persisted = BuildPersistedQueueLocked();
        dirty_ = false;
        lastPersistMs_ = nowMs;
    }

    try {
        persistence_->Save(persisted);
    } catch (const errors::MediaToolException& e) {
        // Losing durability is bad; refusing to keep processing because of it is worse.
        // Log it and carry on with an in-memory queue.
        logging::Log::Error(kLogSubsystem,
                            "queue state could not be saved: " + e.Info().message + " (" +
                                e.Info().details + ")");
        std::lock_guard<std::mutex> lock(mutex_);
        dirty_ = true;
    }
}

void JobManager::FlushPersistence() {
    PersistIfDue(NowMs(), /*force=*/true);
}

JobManager::RecoveryReport JobManager::RestoreFromDisk() {
    RecoveryReport report;
    if (persistence_ == nullptr) return report;

    const queue::LoadOutcome outcome = persistence_->Load();
    report.status = outcome.status;
    report.diagnostic = outcome.diagnostic;
    report.quarantinedPath = outcome.quarantinedPath;

    if (outcome.status == queue::LoadOutcome::Status::Recovered) {
        logging::Log::Error(kLogSubsystem, "queue state recovery: " + outcome.diagnostic +
                                               (outcome.quarantinedPath
                                                    ? " (kept at " + *outcome.quarantinedPath + ")"
                                                    : ""));
        return report;
    }
    if (outcome.status == queue::LoadOutcome::Status::NotPresent) return report;

    std::vector<queue::JobRecord> records = outcome.queue.records;
    const auto interrupted = queue::ApplyRestartRecovery(records);
    report.interruptedJobs = static_cast<int>(interrupted.size());
    for (const auto& id : interrupted)
        logging::Log::Warning(kLogSubsystem, "job " + id + " was interrupted by a shutdown");

    // Records are restored in the persisted pending order first so dependencies and queue
    // positions land the way they were saved; SchedulerCore::Insert rejects a dependency it
    // has not seen yet, so order genuinely matters here.
    std::vector<queue::JobRecord> ordered;
    for (const auto& id : outcome.queue.pendingOrder) {
        const auto it = std::find_if(records.begin(), records.end(),
                                     [&id](const queue::JobRecord& r) { return r.id == id; });
        if (it != records.end()) ordered.push_back(*it);
    }
    for (const auto& record : records) {
        if (std::find_if(ordered.begin(), ordered.end(), [&record](const queue::JobRecord& r) {
                return r.id == record.id;
            }) == ordered.end()) {
            ordered.push_back(record);
        }
    }
    // Dependencies must exist before their dependents. A stable partition by dependency
    // count is not sufficient in general, so do a simple topological pass and append
    // anything left over (a cycle, which Insert will then reject individually).
    std::vector<queue::JobRecord> sorted;
    std::vector<bool> placed(ordered.size(), false);
    bool progressed = true;
    while (progressed) {
        progressed = false;
        for (std::size_t i = 0; i < ordered.size(); ++i) {
            if (placed[i]) continue;
            const bool ready = std::all_of(
                ordered[i].dependencies.begin(), ordered[i].dependencies.end(),
                [&sorted](const JobId& dep) {
                    return std::any_of(sorted.begin(), sorted.end(),
                                       [&dep](const queue::JobRecord& r) { return r.id == dep; });
                });
            if (!ready) continue;
            sorted.push_back(ordered[i]);
            placed[i] = true;
            progressed = true;
        }
    }
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        if (!placed[i]) sorted.push_back(ordered[i]);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        scheduler_.SetRunState(outcome.queue.runState);
        scheduler_.SetMaxConcurrency(outcome.queue.maxConcurrency);
    }

    for (const auto& record : sorted) {
        std::unique_ptr<Job> job;
        if (factory_ != nullptr) {
            try {
                job = factory_->Create(record.spec);
            } catch (const errors::MediaToolException& e) {
                logging::Log::Warning(kLogSubsystem, "could not rebuild job " + record.id + ": " +
                                                         e.Info().message);
            }
        }
        if (job == nullptr) {
            ++report.unbuildableJobs;
            continue;
        }

        try {
            job->AdoptRestoredId(record.id);
        } catch (const errors::MediaToolException&) {
            ++report.unbuildableJobs;
            continue;
        }

        std::shared_ptr<Job> shared(std::move(job));
        queue::JobRecord restored = record;
        // Terminal records are re-inserted as pending and immediately driven to their
        // persisted terminal state below: SchedulerCore::Insert only admits pending jobs,
        // and history has to come back so the user still sees what happened.
        const JobState persistedState = restored.state;
        restored.state = JobState::Queued;

        try {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                scheduler_.Insert(restored, NowMs());
                jobs_.emplace(record.id, shared);
            }
        } catch (const errors::MediaToolException& e) {
            logging::Log::Warning(kLogSubsystem,
                                  "could not restore job " + record.id + ": " + e.Info().message);
            ++report.unbuildableJobs;
            continue;
        }

        const JobId id = record.id;
        shared->SetCallbacks(
            [this, id](JobState state) { HandleJobStateChanged(id, state); },
            [this, id](const Progress& progress) { HandleJobProgress(id, progress); });

        try {
            switch (persistedState) {
                case JobState::Waiting:
                    shared->MarkWaiting();
                    break;
                case JobState::RetryWait:
                    // Restored via Failed, the only legal predecessor of RetryWait.
                    shared->MarkStarting();
                    shared->MarkFailed(errors::ErrorInfo::Make(
                        "E_JOB_INTERRUPTED", errors::ErrorCategory::Unknown,
                        "This job was waiting to retry when the app closed.", "", true));
                    shared->MarkRetryWait();
                    break;
                case JobState::Failed:
                    shared->MarkStarting();
                    shared->MarkFailed(errors::ErrorInfo::Make(
                        "E_JOB_INTERRUPTED", errors::ErrorCategory::Unknown,
                        "This job was interrupted when the app closed. Retry to run it again.",
                        restored.lastRetryReason, true));
                    break;
                case JobState::Completed:
                    shared->MarkStarting();
                    shared->MarkRunning();
                    shared->MarkCompleted();
                    break;
                case JobState::Cancelled:
                    shared->RequestCancel();
                    break;
                case JobState::Skipped:
                    shared->MarkSkipped(errors::ErrorInfo::Make(
                        "E_DEPENDENCY_FAILED", errors::ErrorCategory::Unknown,
                        "Skipped because a job it depended on did not finish."));
                    break;
                default:
                    break;  // Queued stays Queued
            }
        } catch (const errors::MediaToolException& e) {
            logging::Log::Warning(kLogSubsystem, "could not restore state for " + record.id + ": " +
                                                     e.Info().message);
        }

        // Re-apply the fields Insert() reset, now that the state has settled.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue::JobRecord* live = scheduler_.FindMutable(record.id)) {
                live->attempt = record.attempt;
                live->nextRetryAtMs = record.nextRetryAtMs;
                live->lastRetryReason = record.lastRetryReason;
                live->createdAtMs = record.createdAtMs;
                live->sequence = record.sequence;
                live->revision = record.revision + 1;
                if (live->state == JobState::RetryWait && !live->nextRetryAtMs)
                    live->nextRetryAtMs = NowMs();
            }
        }
        ++report.restoredJobs;
    }

    // Restore the saved queue order, which Insert()'s append-to-tail behaviour did not
    // preserve for jobs that came back out of order.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = outcome.queue.pendingOrder.rbegin(); it != outcome.queue.pendingOrder.rend();
             ++it) {
            try {
                scheduler_.Move(*it, queue::MoveDirection::Top);
            } catch (const errors::MediaToolException&) {
                // No longer pending (it finished, or never came back). Nothing to order.
            }
        }
        dirty_ = true;
    }

    logging::Log::Info(kLogSubsystem,
                       "restored " + std::to_string(report.restoredJobs) + " jobs (" +
                           std::to_string(report.interruptedJobs) + " interrupted, " +
                           std::to_string(report.unbuildableJobs) + " unbuildable)");
    NotifyQueueChanged();
    WakeScheduler();
    return report;
}

std::vector<std::string> JobManager::ValidateInvariants() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto violations = scheduler_.ValidateInvariants();

    // Cross-check the two halves of the system against each other: a record whose state has
    // drifted from its Job's is the bug class this whole design is trying to make impossible.
    for (const auto& record : scheduler_.Snapshot()) {
        const auto it = jobs_.find(record.id);
        if (it == jobs_.end()) {
            violations.push_back("record " + record.id + " has no Job object");
            continue;
        }
        const JobState jobState = it->second->State();
        if (jobState != record.state) {
            violations.push_back("job " + record.id + " is " + ToWireString(jobState) +
                                 " but its record says " + ToWireString(record.state));
        }
    }
    for (const auto& [id, job] : jobs_) {
        if (!scheduler_.Contains(id)) violations.push_back("job " + id + " has no scheduler record");
    }
    return violations;
}

}  // namespace mediatool::jobs
