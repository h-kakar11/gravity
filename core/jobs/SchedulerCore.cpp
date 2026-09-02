#include "core/jobs/SchedulerCore.h"

#include <algorithm>
#include <utility>

#include "core/errors/ErrorInfo.h"
#include "core/errors/MediaToolException.h"

namespace mediatool::jobs {

namespace {

[[noreturn]] void ThrowInvalidDependency(const JobId& id, const JobId& dependency,
                                          const std::string& why) {
    throw errors::MediaToolException(errors::ErrorInfo::Make(
        "E_INVALID_DEPENDENCY", errors::ErrorCategory::Unknown,
        "This job depends on another job that cannot satisfy it.",
        "job=" + id + " dependsOn=" + dependency + " reason=" + why));
}

}  // namespace

void SchedulerCore::Submit(Submission submission) {
    if (entries_.count(submission.id)) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_DUPLICATE_JOB", errors::ErrorCategory::Unknown, "This job is already scheduled.",
            "job=" + submission.id));
    }

    // Validate the whole dependency list before mutating anything: a Submit that throws
    // must leave the scheduler exactly as it found it, or a rejected submission would
    // corrupt the graph it was rejected for the sake of.
    for (const JobId& dependency : submission.dependsOn) {
        if (dependency == submission.id) {
            ThrowInvalidDependency(submission.id, dependency, "a job cannot depend on itself");
        }
        const auto it = entries_.find(dependency);
        if (it == entries_.end()) {
            // This is also the cycle guard: an id that does not exist yet cannot be
            // depended on, so no edge can ever point forwards, so no cycle can form.
            ThrowInvalidDependency(submission.id, dependency,
                                    "no such job (a dependency must already be submitted)");
        }
        if (it->second.phase == Phase::Finished && it->second.terminal != JobState::Completed) {
            ThrowInvalidDependency(submission.id, dependency,
                                    "that job finished as " + ToWireString(it->second.terminal) +
                                        " and will never complete");
        }
    }

    // `runAfter` is validated more permissively than `dependsOn` on purpose: a predecessor
    // that already finished as FAILED/CANCELLED is a *satisfied* sequencing edge (it is no
    // longer going to run, so nothing is being waited for), whereas the same state makes a
    // `dependsOn` edge permanently unsatisfiable. Existence and non-self-reference still
    // hold -- they are what keep the graph acyclic.
    for (const JobId& predecessor : submission.runAfter) {
        if (predecessor == submission.id) {
            ThrowInvalidDependency(submission.id, predecessor, "a job cannot run after itself");
        }
        if (!entries_.count(predecessor)) {
            ThrowInvalidDependency(submission.id, predecessor,
                                    "no such job (a runAfter must already be submitted)");
        }
    }

    Entry entry;
    entry.key = {submission.priority, nextSequence_++};
    entry.dependsOn = submission.dependsOn;
    entry.runAfter = submission.runAfter;
    entry.phase = Phase::Pending;

    for (const JobId& dependency : entry.dependsOn) {
        auto& reverse = dependents_[dependency];
        // A duplicate edge (dependsOn: [A, A]) must not produce a duplicate dependent, or
        // RecordTerminal would report the same job twice.
        if (std::find(reverse.begin(), reverse.end(), submission.id) == reverse.end()) {
            reverse.push_back(submission.id);
        }
    }

    const PendingKey key = entry.key;
    entries_.emplace(submission.id, std::move(entry));
    pending_.emplace(key, submission.id);
}

bool SchedulerCore::DependenciesSatisfied(const JobId& id) const {
    const Entry& entry = entries_.at(id);
    for (const JobId& dependency : entry.dependsOn) {
        const auto it = entries_.find(dependency);
        // A forgotten dependency cannot be waited on -- Forget() refuses to drop a job with
        // pending dependents precisely so this cannot happen, so treat it as satisfied
        // rather than deadlocking the dependent forever if it somehow does.
        if (it == entries_.end()) continue;
        if (it->second.phase != Phase::Finished) return false;
        if (it->second.terminal != JobState::Completed) return false;
    }
    // Sequencing edges: finished is enough, whatever the outcome was.
    for (const JobId& predecessor : entry.runAfter) {
        const auto it = entries_.find(predecessor);
        if (it == entries_.end()) continue;  // forgotten; see the note above
        if (it->second.phase != Phase::Finished) return false;
    }
    return true;
}

std::optional<JobId> SchedulerCore::TakeNextEligible(TimePoint now) {
    // Iterates in scheduling order; the first eligible entry wins, and the ones passed
    // over stay exactly where they are.
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        if (!IsEligible(it->second, now)) continue;  // skipped, not blocking
        const JobId id = it->second;
        pending_.erase(it);
        entries_.at(id).phase = Phase::Running;
        entries_.at(id).notBefore = TimePoint{};  // consumed
        return id;
    }
    return std::nullopt;
}

bool SchedulerCore::HasEligible(TimePoint now) const {
    return std::any_of(pending_.begin(), pending_.end(),
                        [this, now](const auto& entry) { return IsEligible(entry.second, now); });
}

std::optional<SchedulerCore::TimePoint> SchedulerCore::NextEligibleTime(TimePoint now) const {
    std::optional<TimePoint> earliest;
    for (const auto& [key, id] : pending_) {
        const auto entry = entries_.find(id);
        if (entry == entries_.end()) continue;
        // Only a job held back purely by its own clock can be woken by waiting. One
        // waiting on a dependency becomes eligible when that dependency finishes, which
        // is a notify, not a deadline.
        if (!DependenciesSatisfied(id)) continue;
        if (entry->second.notBefore <= now) continue;  // already eligible, nothing to wait for
        if (!earliest || entry->second.notBefore < *earliest) earliest = entry->second.notBefore;
    }
    return earliest;
}

bool SchedulerCore::IsEligible(const JobId& id, TimePoint now) const {
    const auto entry = entries_.find(id);
    if (entry == entries_.end()) return false;
    if (entry->second.notBefore > now) return false;
    return DependenciesSatisfied(id);
}

std::vector<JobId> SchedulerCore::RecordTerminal(const JobId& id, JobState terminal) {
    const auto it = entries_.find(id);
    if (it == entries_.end()) return {};
    if (it->second.phase == Phase::Finished) return {};  // already recorded; dependents handled

    it->second.phase = Phase::Finished;
    it->second.terminal = terminal;
    // A job cancelled while still queued never ran, but it is no longer pending either.
    pending_.erase(it->second.key);

    if (terminal == JobState::Completed) return {};

    // The dependency did not complete, so everything still waiting on it is unrunnable.
    // Only the direct, still-pending dependents are returned -- see the header.
    std::vector<JobId> unrunnable;
    const auto reverse = dependents_.find(id);
    if (reverse == dependents_.end()) return unrunnable;
    for (const JobId& dependent : reverse->second) {
        const auto dependentEntry = entries_.find(dependent);
        if (dependentEntry == entries_.end()) continue;
        if (dependentEntry->second.phase != Phase::Pending) continue;
        unrunnable.push_back(dependent);
    }
    return unrunnable;
}

void SchedulerCore::Requeue(const JobId& id, int priority, TimePoint notBefore) {
    const auto it = entries_.find(id);
    if (it == entries_.end()) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_JOB_NOT_FOUND", errors::ErrorCategory::Unknown, "Job not found",
            "No job registered with id " + id));
    }
    if (it->second.phase == Phase::Pending) return;

    it->second.phase = Phase::Pending;
    it->second.notBefore = notBefore;
    // A retried job goes to the back of its priority band rather than jumping ahead of
    // jobs that have been waiting: it already had its turn.
    it->second.key = {priority, nextSequence_++};
    pending_.emplace(it->second.key, id);
}

void SchedulerCore::Forget(const JobId& id) {
    const auto it = entries_.find(id);
    if (it == entries_.end()) return;

    const auto reverse = dependents_.find(id);
    if (reverse != dependents_.end()) {
        for (const JobId& dependent : reverse->second) {
            const auto dependentEntry = entries_.find(dependent);
            if (dependentEntry != entries_.end() && dependentEntry->second.phase == Phase::Pending) {
                throw errors::MediaToolException(errors::ErrorInfo::Make(
                    "E_JOB_HAS_DEPENDENTS", errors::ErrorCategory::Unknown,
                    "Another queued job is waiting on this one.",
                    "job=" + id + " blockedDependent=" + dependent));
            }
        }
        dependents_.erase(reverse);
    }

    for (const JobId& dependency : it->second.dependsOn) {
        const auto edges = dependents_.find(dependency);
        if (edges == dependents_.end()) continue;
        edges->second.erase(std::remove(edges->second.begin(), edges->second.end(), id),
                             edges->second.end());
        if (edges->second.empty()) dependents_.erase(edges);
    }

    pending_.erase(it->second.key);
    entries_.erase(it);
}

std::vector<JobId> SchedulerCore::TakeAllPending() {
    std::vector<JobId> taken = PendingOrder();
    pending_.clear();
    return taken;
}

bool SchedulerCore::Knows(const JobId& id) const { return entries_.count(id) != 0; }

bool SchedulerCore::IsPending(const JobId& id) const {
    const auto it = entries_.find(id);
    return it != entries_.end() && it->second.phase == Phase::Pending;
}

std::size_t SchedulerCore::PendingCount() const { return pending_.size(); }

std::vector<JobId> SchedulerCore::PendingOrder() const {
    std::vector<JobId> order;
    order.reserve(pending_.size());
    for (const auto& [key, id] : pending_) order.push_back(id);
    return order;
}

}  // namespace mediatool::jobs
