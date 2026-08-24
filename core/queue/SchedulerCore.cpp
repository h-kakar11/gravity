#include "core/queue/SchedulerCore.h"

#include <algorithm>
#include <functional>
#include <unordered_set>
#include <utility>

#include "core/errors/MediaToolException.h"

namespace mediatool::queue {

namespace {

using errors::ErrorCategory;
using errors::ErrorInfo;
using errors::MediaToolException;

[[noreturn]] void Throw(const std::string& code, const std::string& message,
                        const std::string& details = "") {
    throw MediaToolException(ErrorInfo::Make(code, ErrorCategory::Unknown, message, details));
}

}  // namespace

void SchedulerCore::SetMaxConcurrency(std::size_t value) {
    // Zero would wedge the queue with no way to notice from the outside; treat it the same
    // way JobManager has always treated a zero worker count.
    config_.maxConcurrency = value == 0 ? 1 : value;
}

const JobRecord* SchedulerCore::Find(const jobs::JobId& id) const {
    const auto it = records_.find(id);
    return it == records_.end() ? nullptr : &it->second;
}

JobRecord* SchedulerCore::FindMutable(const jobs::JobId& id) {
    const auto it = records_.find(id);
    return it == records_.end() ? nullptr : &it->second;
}

bool SchedulerCore::IsPendingState(jobs::JobState state) const {
    return state == jobs::JobState::Queued || state == jobs::JobState::Waiting ||
           state == jobs::JobState::RetryWait;
}

void SchedulerCore::AddToPending(const jobs::JobId& id) {
    if (std::find(pendingOrder_.begin(), pendingOrder_.end(), id) == pendingOrder_.end())
        pendingOrder_.push_back(id);
}

void SchedulerCore::RemoveFromPending(const jobs::JobId& id) {
    pendingOrder_.erase(std::remove(pendingOrder_.begin(), pendingOrder_.end(), id),
                        pendingOrder_.end());
}

std::optional<jobs::JobId> SchedulerCore::FindActiveDuplicate(const std::string& key) const {
    if (key.empty()) return std::nullopt;
    for (const auto& [id, record] : records_) {
        if (record.duplicateKey == key && jobs::IsActiveState(record.state)) return id;
    }
    return std::nullopt;
}

void SchedulerCore::RejectCycles(const JobRecord& record) const {
    // Walk up from each declared dependency; if the walk reaches the new job's own id, the
    // edge we are about to add closes a loop. Done before insertion so a rejected job
    // leaves no trace.
    std::unordered_set<jobs::JobId> visited;
    std::function<void(const jobs::JobId&)> visit = [&](const jobs::JobId& current) {
        if (current == record.id) {
            Throw("E_DEPENDENCY_CYCLE", "These jobs would depend on each other in a loop.",
                  "cycle involves " + record.id);
        }
        if (!visited.insert(current).second) return;
        const auto it = records_.find(current);
        if (it == records_.end()) return;
        for (const auto& dep : it->second.dependencies) visit(dep);
    };
    for (const auto& dep : record.dependencies) visit(dep);
}

void SchedulerCore::Insert(JobRecord record, std::int64_t nowMs) {
    if (record.id.empty()) Throw("E_INVALID_JOB_ID", "A job id is required.");
    if (records_.count(record.id) > 0)
        Throw("E_DUPLICATE_JOB_ID", "A job with this id already exists.", record.id);

    // Validate the whole dependency list before touching any state: a half-inserted job
    // with some edges applied would be worse than a clean rejection.
    std::unordered_set<jobs::JobId> seen;
    for (const auto& dep : record.dependencies) {
        if (dep == record.id)
            Throw("E_DEPENDENCY_INVALID", "A job cannot depend on itself.", record.id);
        if (records_.count(dep) == 0)
            Throw("E_DEPENDENCY_NOT_FOUND", "This job depends on a job that does not exist.",
                  "missing dependency " + dep);
        if (!seen.insert(dep).second)
            Throw("E_DEPENDENCY_INVALID", "The same dependency was listed twice.", dep);
    }
    RejectCycles(record);

    if (const auto existing = FindActiveDuplicate(record.duplicateKey)) {
        Throw("E_DUPLICATE_JOB",
              "An identical job is already in the queue.", "existing job " + *existing);
    }

    if (record.sequence == 0) record.sequence = NextSequence();
    sequenceCounter_ = std::max(sequenceCounter_, record.sequence);
    if (record.createdAtMs == 0) record.createdAtMs = nowMs;
    record.pendingSinceMs = nowMs;

    // A job with unmet dependencies must never sit in Queued -- Queued means "runnable the
    // moment a slot frees up", and the scheduler relies on that.
    const bool blocked = std::any_of(
        record.dependencies.begin(), record.dependencies.end(), [this](const jobs::JobId& dep) {
            const auto it = records_.find(dep);
            return it == records_.end() || it->second.state != jobs::JobState::Completed;
        });
    record.state = blocked ? jobs::JobState::Waiting : jobs::JobState::Queued;
    ++record.revision;

    const jobs::JobId id = record.id;
    records_.emplace(id, std::move(record));
    AddToPending(id);
}

void SchedulerCore::SetState(const jobs::JobId& id, jobs::JobState state, std::int64_t nowMs) {
    JobRecord* record = FindMutable(id);
    if (record == nullptr) return;
    if (record->state == state) return;

    const bool wasPending = IsPendingState(record->state);
    const bool nowPending = IsPendingState(state);

    record->state = state;
    ++record->revision;

    if (nowPending && !wasPending) {
        // Re-entering the pending set (a retry, or a dependency regressing a Queued job
        // back to Waiting). Aging restarts from here rather than from creation, so a job
        // that has already had a turn does not arrive pre-aged and jump the line.
        record->pendingSinceMs = nowMs;
        AddToPending(id);
    } else if (!nowPending && wasPending) {
        RemoveFromPending(id);
    }

    if (jobs::IsTerminalState(state)) {
        record->finishedAtMs = nowMs;
        record->nextRetryAtMs.reset();
        RecordTerminal(id);
        EnforceHistoryLimit();
    } else {
        // Leaving a terminal state (manual retry) means this is no longer history.
        record->finishedAtMs.reset();
        historyOrder_.erase(std::remove(historyOrder_.begin(), historyOrder_.end(), id),
                            historyOrder_.end());
    }
}

void SchedulerCore::RecordTerminal(const jobs::JobId& id) {
    historyOrder_.erase(std::remove(historyOrder_.begin(), historyOrder_.end(), id),
                        historyOrder_.end());
    historyOrder_.push_back(id);
}

void SchedulerCore::EnforceHistoryLimit() {
    if (config_.historyLimit == 0) return;
    while (historyOrder_.size() > config_.historyLimit) {
        // Failures are the entries a user actually needs later, so evict a successful or
        // cancelled job first and only start dropping failures once nothing else is left
        // (spec section 26).
        auto victim = std::find_if(historyOrder_.begin(), historyOrder_.end(),
                                   [this](const jobs::JobId& candidate) {
                                       const auto it = records_.find(candidate);
                                       return it != records_.end() &&
                                              it->second.state != jobs::JobState::Failed;
                                   });
        if (victim == historyOrder_.end()) victim = historyOrder_.begin();
        records_.erase(*victim);
        historyOrder_.erase(victim);
    }
}

bool SchedulerCore::IsEligible(const JobRecord& record, std::int64_t nowMs) const {
    if (record.state == jobs::JobState::Queued) {
        // Belt and braces: Queued is supposed to imply satisfied dependencies, and
        // ValidateInvariants enforces that, but a job must never start early even if some
        // future caller breaks that rule.
        return std::all_of(record.dependencies.begin(), record.dependencies.end(),
                           [this](const jobs::JobId& dep) {
                               const auto it = records_.find(dep);
                               return it != records_.end() &&
                                      it->second.state == jobs::JobState::Completed;
                           });
    }
    if (record.state == jobs::JobState::RetryWait) {
        return record.nextRetryAtMs.has_value() && nowMs >= *record.nextRetryAtMs;
    }
    return false;
}

int SchedulerCore::EffectivePriority(const JobRecord& record, std::int64_t nowMs) const {
    const int base = PriorityRank(record.priority);
    if (config_.agingIntervalMs <= 0) return base;
    const std::int64_t waited = nowMs - record.pendingSinceMs;
    if (waited <= 0) return base;
    const std::int64_t steps = waited / config_.agingIntervalMs;
    const int boost = static_cast<int>(
        std::min<std::int64_t>(steps, static_cast<std::int64_t>(config_.maxAgingBoost)));
    return base + boost;
}

std::size_t SchedulerCore::RunningCount() const {
    return static_cast<std::size_t>(
        std::count_if(records_.begin(), records_.end(), [](const auto& entry) {
            return jobs::IsExecutingState(entry.second.state);
        }));
}

std::vector<jobs::JobId> SchedulerCore::SelectDispatchable(std::int64_t nowMs) {
    if (runState_ == QueueRunState::Paused) return {};

    const std::size_t running = RunningCount();
    if (running >= config_.maxConcurrency) return {};
    std::size_t slots = config_.maxConcurrency - running;

    // Candidates keep their pending-order index so it can break priority ties -- that index
    // is what "move to top" actually manipulates.
    struct Candidate {
        jobs::JobId id;
        int priority;
        std::size_t position;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(pendingOrder_.size());
    for (std::size_t position = 0; position < pendingOrder_.size(); ++position) {
        const JobRecord* record = Find(pendingOrder_[position]);
        if (record == nullptr || !IsEligible(*record, nowMs)) continue;
        candidates.push_back({record->id, EffectivePriority(*record, nowMs), position});
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.position < b.position;
    });

    std::vector<jobs::JobId> dispatched;
    for (const auto& candidate : candidates) {
        if (slots == 0) break;
        // Marking Starting here (rather than leaving it to the caller) is what makes two
        // consecutive SelectDispatchable calls safe: the job is out of the pending set and
        // counted as running before this function returns.
        SetState(candidate.id, jobs::JobState::Starting, nowMs);
        dispatched.push_back(candidate.id);
        --slots;
    }
    return dispatched;
}

std::vector<PendingTransition> SchedulerCore::ResolveDependencies(std::int64_t nowMs) {
    std::vector<PendingTransition> transitions;

    for (auto& [id, record] : records_) {
        if (record.state != jobs::JobState::Waiting && record.state != jobs::JobState::Queued)
            continue;
        if (record.dependencies.empty()) continue;

        bool allComplete = true;
        const JobRecord* blocker = nullptr;
        for (const auto& dep : record.dependencies) {
            const auto it = records_.find(dep);
            if (it == records_.end()) {
                // The dependency was cleared from history out from under us. Treat it as
                // unsatisfiable rather than silently running the dependent.
                blocker = nullptr;
                allComplete = false;
                transitions.push_back(
                    {id, jobs::JobState::Skipped,
                     ErrorInfo::Make("E_DEPENDENCY_FAILED", ErrorCategory::Unknown,
                                     "A job this one depended on is no longer available.",
                                     "missing dependency " + dep)});
                break;
            }
            const jobs::JobState depState = it->second.state;
            if (depState == jobs::JobState::Completed) continue;
            allComplete = false;
            if (depState == jobs::JobState::Failed || depState == jobs::JobState::Cancelled ||
                depState == jobs::JobState::Skipped) {
                blocker = &it->second;
                break;
            }
        }

        if (blocker != nullptr) {
            transitions.push_back(
                {id, jobs::JobState::Skipped,
                 ErrorInfo::Make("E_DEPENDENCY_FAILED", ErrorCategory::Unknown,
                                 "Skipped because a job it depended on did not finish.",
                                 "dependency " + blocker->id + " ended as " +
                                     jobs::ToWireString(blocker->state))});
            continue;
        }
        if (allComplete && record.state == jobs::JobState::Waiting) {
            transitions.push_back({id, jobs::JobState::Queued, std::nullopt});
        } else if (!allComplete && record.state == jobs::JobState::Queued) {
            // A dependency went un-terminal again (it was manually retried) underneath a
            // job that had already been cleared to run.
            transitions.push_back({id, jobs::JobState::Waiting, std::nullopt});
        }
    }

    for (const auto& transition : transitions) SetState(transition.id, transition.newState, nowMs);
    return transitions;
}

std::optional<std::int64_t> SchedulerCore::NextWakeupMs(std::int64_t nowMs) const {
    // Only retry deadlines are time-gated. Aging changes selection *order*, not whether
    // anything can run, so it never needs a wakeup of its own: if a slot is free the
    // scheduler dispatches now, and if none is free a wakeup would find nothing to do.
    std::optional<std::int64_t> earliest;
    for (const auto& id : pendingOrder_) {
        const JobRecord* record = Find(id);
        if (record == nullptr || record->state != jobs::JobState::RetryWait) continue;
        if (!record->nextRetryAtMs) continue;
        if (*record->nextRetryAtMs <= nowMs) return nowMs;  // already due
        earliest = earliest ? std::min(*earliest, *record->nextRetryAtMs) : *record->nextRetryAtMs;
    }
    return earliest;
}

bool SchedulerCore::HasRetryBudget(const jobs::JobId& id) const {
    const JobRecord* record = Find(id);
    if (record == nullptr) return false;
    return record->attempt < record->retryPolicy.maxRetries;
}

void SchedulerCore::ScheduleRetry(const jobs::JobId& id, std::int64_t delayMs, std::int64_t nowMs,
                                  std::string reason) {
    JobRecord* record = FindMutable(id);
    if (record == nullptr) return;
    ++record->attempt;
    record->nextRetryAtMs = nowMs + std::max<std::int64_t>(0, delayMs);
    record->lastRetryReason = std::move(reason);
    // Re-enters the pending order at the tail: this job has already had a turn, so work
    // queued while it was running gets its turn first. Priority still applies on top of
    // that, so a HIGH retry does not sit behind NORMAL work (docs/phase-5.md).
    SetState(id, jobs::JobState::RetryWait, nowMs);
}

void SchedulerCore::PrepareManualRetry(const jobs::JobId& id, std::int64_t nowMs) {
    JobRecord* record = FindMutable(id);
    if (record == nullptr) Throw("E_JOB_NOT_FOUND", "Job not found.", id);
    ++record->attempt;
    // Due immediately rather than "no deadline": a manual retry reuses the exact same
    // RetryWait -> Retrying -> Running path an automatic one takes, it just skips the wait.
    // One dispatch path instead of two is what keeps "a job never runs twice" tractable.
    record->nextRetryAtMs = nowMs;
    record->lastRetryReason = "manual retry requested";
    ++record->revision;
    SetState(id, jobs::JobState::RetryWait, nowMs);
}

void SchedulerCore::Move(const jobs::JobId& id, MoveDirection direction) {
    const JobRecord* record = Find(id);
    if (record == nullptr) Throw("E_JOB_NOT_FOUND", "Job not found.", id);
    const auto it = std::find(pendingOrder_.begin(), pendingOrder_.end(), id);
    if (it == pendingOrder_.end()) {
        Throw("E_JOB_NOT_REORDERABLE",
              "Only a job that has not started yet can be moved.",
              "job " + id + " is " + jobs::ToWireString(record->state));
    }

    const std::size_t index = static_cast<std::size_t>(std::distance(pendingOrder_.begin(), it));
    switch (direction) {
        case MoveDirection::Top:
            pendingOrder_.erase(it);
            pendingOrder_.insert(pendingOrder_.begin(), id);
            return;
        case MoveDirection::Bottom:
            pendingOrder_.erase(it);
            pendingOrder_.push_back(id);
            return;
        case MoveDirection::Up:
            if (index > 0) std::swap(pendingOrder_[index], pendingOrder_[index - 1]);
            return;
        case MoveDirection::Down:
            if (index + 1 < pendingOrder_.size())
                std::swap(pendingOrder_[index], pendingOrder_[index + 1]);
            return;
    }
}

void SchedulerCore::SetPriority(const jobs::JobId& id, JobPriority priority) {
    JobRecord* record = FindMutable(id);
    if (record == nullptr) Throw("E_JOB_NOT_FOUND", "Job not found.", id);
    if (jobs::IsTerminalState(record->state)) {
        Throw("E_JOB_INVALID_OPERATION", "A finished job's priority cannot be changed.",
              "job " + id + " is " + jobs::ToWireString(record->state));
    }
    record->priority = priority;
    ++record->revision;
}

std::vector<jobs::JobId> SchedulerCore::ClearHistory(HistoryScope scope) {
    const auto matches = [scope](jobs::JobState state) {
        if (!jobs::IsTerminalState(state)) return false;  // never touches live work
        switch (scope) {
            case HistoryScope::Completed: return state == jobs::JobState::Completed;
            case HistoryScope::Failed: return state == jobs::JobState::Failed;
            case HistoryScope::Cancelled: return state == jobs::JobState::Cancelled;
            case HistoryScope::Skipped: return state == jobs::JobState::Skipped;
            case HistoryScope::All: return true;
        }
        return false;
    };

    std::vector<jobs::JobId> removed;
    for (auto it = records_.begin(); it != records_.end();) {
        if (matches(it->second.state)) {
            removed.push_back(it->first);
            it = records_.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& id : removed) {
        historyOrder_.erase(std::remove(historyOrder_.begin(), historyOrder_.end(), id),
                            historyOrder_.end());
    }
    return removed;
}

void SchedulerCore::Remove(const jobs::JobId& id) {
    const JobRecord* record = Find(id);
    if (record == nullptr) Throw("E_JOB_NOT_FOUND", "Job not found.", id);
    if (!jobs::IsTerminalState(record->state)) {
        Throw("E_JOB_INVALID_OPERATION", "Only a finished job can be removed.",
              "job " + id + " is " + jobs::ToWireString(record->state));
    }
    records_.erase(id);
    RemoveFromPending(id);
    historyOrder_.erase(std::remove(historyOrder_.begin(), historyOrder_.end(), id),
                        historyOrder_.end());
}

std::vector<JobRecord> SchedulerCore::Snapshot() const {
    std::vector<JobRecord> result;
    result.reserve(records_.size());

    // Pending jobs first, in the order the user sees and controls...
    std::unordered_set<jobs::JobId> emitted;
    for (const auto& id : pendingOrder_) {
        const JobRecord* record = Find(id);
        if (record == nullptr) continue;
        result.push_back(*record);
        emitted.insert(id);
    }
    // ...then everything else (running and finished) by creation order, which is stable and
    // meaningful in a way that map key order is not.
    std::vector<const JobRecord*> rest;
    for (const auto& [id, record] : records_) {
        if (emitted.count(id) == 0) rest.push_back(&record);
    }
    std::sort(rest.begin(), rest.end(),
              [](const JobRecord* a, const JobRecord* b) { return a->sequence < b->sequence; });
    for (const JobRecord* record : rest) result.push_back(*record);
    return result;
}

QueueStatistics SchedulerCore::Stats() const {
    QueueStatistics stats;
    for (const auto& [id, record] : records_) {
        ++stats.total;
        switch (record.state) {
            case jobs::JobState::Queued: ++stats.queued; break;
            case jobs::JobState::Waiting: ++stats.waiting; break;
            case jobs::JobState::Starting:
            case jobs::JobState::Running:
            case jobs::JobState::Retrying: ++stats.running; break;
            case jobs::JobState::Paused: ++stats.paused; break;
            case jobs::JobState::RetryWait: ++stats.retryWait; break;
            case jobs::JobState::Completed: ++stats.completed; break;
            case jobs::JobState::Failed: ++stats.failed; break;
            case jobs::JobState::Cancelled: ++stats.cancelled; break;
            case jobs::JobState::Skipped: ++stats.skipped; break;
        }
    }
    return stats;
}

std::vector<jobs::JobId> SchedulerCore::DependentsOf(const jobs::JobId& id) const {
    std::vector<jobs::JobId> dependents;
    for (const auto& [candidateId, record] : records_) {
        if (std::find(record.dependencies.begin(), record.dependencies.end(), id) !=
            record.dependencies.end()) {
            dependents.push_back(candidateId);
        }
    }
    return dependents;
}

std::vector<std::string> SchedulerCore::ValidateInvariants() const {
    std::vector<std::string> violations;

    std::unordered_set<jobs::JobId> pendingSeen;
    for (const auto& id : pendingOrder_) {
        if (!pendingSeen.insert(id).second)
            violations.push_back("job " + id + " appears twice in the pending order");
        const JobRecord* record = Find(id);
        if (record == nullptr) {
            violations.push_back("pending order references unknown job " + id);
            continue;
        }
        if (!IsPendingState(record->state)) {
            violations.push_back("job " + id + " is in the pending order but is " +
                                 jobs::ToWireString(record->state));
        }
    }

    for (const auto& [id, record] : records_) {
        if (record.id != id) violations.push_back("record key " + id + " != record id " + record.id);
        if (IsPendingState(record.state) && pendingSeen.count(id) == 0)
            violations.push_back("job " + id + " is " + jobs::ToWireString(record.state) +
                                 " but is missing from the pending order");
        if (record.attempt < 0) violations.push_back("job " + id + " has a negative attempt count");
        if (record.attempt > record.retryPolicy.maxRetries + 1)
            violations.push_back("job " + id + " exceeded its retry budget");
        if (record.state == jobs::JobState::RetryWait && !record.nextRetryAtMs)
            violations.push_back("job " + id + " is RETRY_WAIT with no scheduled retry time");
        if (record.state == jobs::JobState::Queued) {
            for (const auto& dep : record.dependencies) {
                const auto it = records_.find(dep);
                if (it != records_.end() && it->second.state != jobs::JobState::Completed) {
                    violations.push_back("job " + id + " is QUEUED but dependency " + dep +
                                         " is " + jobs::ToWireString(it->second.state));
                }
            }
        }
        for (const auto& dep : record.dependencies) {
            if (dep == id) violations.push_back("job " + id + " depends on itself");
        }
    }

    if (RunningCount() > config_.maxConcurrency) {
        violations.push_back("running count " + std::to_string(RunningCount()) +
                             " exceeds concurrency limit " +
                             std::to_string(config_.maxConcurrency));
    }

    // Cycle check across the whole graph, not just the edge most recently added.
    std::unordered_set<jobs::JobId> visiting;
    std::unordered_set<jobs::JobId> done;
    std::function<void(const jobs::JobId&)> visit = [&](const jobs::JobId& current) {
        if (done.count(current) > 0) return;
        if (!visiting.insert(current).second) {
            violations.push_back("dependency cycle involving job " + current);
            return;
        }
        const auto it = records_.find(current);
        if (it != records_.end()) {
            for (const auto& dep : it->second.dependencies) visit(dep);
        }
        visiting.erase(current);
        done.insert(current);
    };
    for (const auto& [id, record] : records_) visit(id);

    return violations;
}

}  // namespace mediatool::queue
