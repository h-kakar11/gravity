#pragma once

// Persists a live snapshot of every non-terminal job (Queued/Starting/Running/
// Paused/Retrying), updated on each state transition, so an in-flight job's last-known
// record survives a crash or kill instead of vanishing with no trace (spec/audit #10).
// Follows the exact atomic-write-then-rename, fallback-to-empty-on-corrupt pattern
// core/settings/JsonFileSettingsStore.h and core/jobs/JobHistoryStore.h already
// establish. Keyed by job id (an object on disk, not JobHistoryStore's append-only
// array) since entries are upserted in place and removed once a job reaches a terminal
// state -- JobHistoryStore is the durable record from that point on, this store's job
// for that id is done.
//
// Scope note: this makes the *record* durable (id, type, state, progress, error,
// timestamps -- the full JobManager::JobSnapshot shape) so a restart can show "N jobs
// were interrupted last session" instead of silent data loss. It does not reconcile
// orphaned on-disk artifacts (the temp-marked partial files Phase B's crash-safety work
// introduced) or resurrect a job back into the live JobManager queue -- both need
// additional per-job-type data (output directory, filename base) the generic snapshot
// doesn't carry, and are tracked as follow-up work in docs/decisions.md.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mediatool::jobs {

// "%LOCALAPPDATA%\Gravity\jobs_in_progress.json". Reads LOCALAPPDATA from the
// environment; never throws.
std::string DefaultInProgressJobsFilePath();

class InProgressJobStore {
public:
    explicit InProgressJobStore(std::string filePath);

    // Every entry currently on disk (JobManager::JobSnapshot::ToJson() shapes), order
    // not guaranteed. Returns an empty vector (never throws, just logs a warning) if the
    // file doesn't exist yet or is corrupt/unreadable -- this is a best-effort recovery
    // aid, never allowed to affect whether the app starts.
    std::vector<nlohmann::json> Load() const;

    // Upserts one job's current snapshot, matched by its "id" field (replaces any
    // existing entry for that id). Called on every non-terminal state transition.
    // Best-effort: a write failure is logged, never thrown -- this runs on the same
    // job-event path JobHistoryStore::Append does and must never disrupt it.
    void Upsert(nlohmann::json snapshotJson);

    // Removes one job's entry once it reaches a terminal state. Best-effort, same as
    // Upsert.
    void Remove(const std::string& jobId);

private:
    void WriteAll(const std::vector<nlohmann::json>& entries) const;

    std::string filePath_;
};

}  // namespace mediatool::jobs
