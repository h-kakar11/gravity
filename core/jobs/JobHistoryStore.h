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
    std::string filePath_;
    std::size_t maxEntries_;
};

}  // namespace mediatool::jobs
