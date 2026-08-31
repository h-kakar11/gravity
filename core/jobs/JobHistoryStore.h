#pragma once

// Persists terminal-state job snapshots across restarts -- the backing store for the
// "Session History" must-have feature (a "recently used" list of downloads/conversions).
// Follows the exact atomic-write-then-rename, fallback-to-empty-on-corrupt pattern
// core/settings/JsonFileSettingsStore.h already establishes; deliberately a bounded ring
// buffer of *terminal* snapshots only, not a general job-persistence mechanism -- resuming
// an in-flight ffmpeg/yt-dlp process across a restart is high complexity for little value
// on jobs that are typically seconds to minutes long, and is an explicit non-goal. A
// restart with active jobs loses them, same as before this store existed.

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mediatool::jobs {

// "%LOCALAPPDATA%\Gravity\job_history.json". Reads LOCALAPPDATA from the environment;
// never throws.
std::string DefaultJobHistoryFilePath();

class JobHistoryStore {
public:
    // `maxEntries` bounds the ring buffer -- Append() past this drops the oldest entries.
    explicit JobHistoryStore(std::string filePath, std::size_t maxEntries = 500);

    // Oldest-first, most-recently-appended last. Returns an empty vector (never throws,
    // just logs a warning) if the file doesn't exist yet or is corrupt/unreadable --
    // history is a convenience feature, never allowed to affect whether the app starts.
    std::vector<nlohmann::json> Load() const;

    // Appends `snapshotJson` (a JobManager::JobSnapshot::ToJson() result) to the stored
    // history, trims to `maxEntries_` from the front if needed, and writes atomically.
    // Best-effort: a write failure is logged, never thrown -- this is called from the job
    // -completion path and must never be allowed to disrupt it.
    void Append(nlohmann::json snapshotJson);

private:
    // Append() is a read-modify-write of a single file and it is called from JobManager's
    // worker threads (via the job state-changed callback), so at concurrentJobs > 1 two
    // jobs finishing at the same moment used to interleave: both read the same history,
    // both append their own entry to it, and the second rename silently discarded the
    // first job's entry. Serializing the whole read-modify-write is what makes "every
    // terminal job is recorded" true rather than probable. Load() takes it too, so a
    // reader never observes the file mid-rename.
    mutable std::mutex mutex_;
    std::string filePath_;
    std::size_t maxEntries_;

    // Load() without the lock, for Append()'s use once it already holds it (std::mutex is
    // not recursive).
    std::vector<nlohmann::json> LoadLocked() const;
};

}  // namespace mediatool::jobs
