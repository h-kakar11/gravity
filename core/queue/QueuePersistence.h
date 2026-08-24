#pragma once

// Durable queue state: what survives closing the app (spec sections 22-25).
//
// Format is a single versioned JSON document written through filesystem::AtomicWriter, so
// a crash mid-write leaves the previous good file untouched rather than a truncated one.
// AtomicWriter was evaluated for this and is the right tool: it already implements exactly
// the write-temp / fsync-by-close / rename-over pattern this needs, and its destructor
// cleans up the temp file on any error path (spec section 23).
//
// What is persisted is JobRecords -- scheduling metadata plus the JobSpec needed to rebuild
// a Job. Live process handles, OS PIDs, progress percentages, and open file descriptors are
// all deliberately excluded: none of them mean anything in the next process.
//
// Loading NEVER throws for a bad file. A corrupt, truncated, empty, or future-versioned
// state file yields an empty queue plus a diagnostic, and the bad file is moved aside
// rather than deleted so it can still be inspected (spec section 24). Losing the queue is
// survivable; refusing to start is not.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/queue/JobRecord.h"
#include "core/queue/QueueTypes.h"

namespace mediatool::queue {

// Bump when the on-disk shape changes incompatibly, and add a migration in Migrate().
inline constexpr int kQueueSchemaVersion = 1;

struct PersistedQueue {
    int schemaVersion = kQueueSchemaVersion;
    QueueRunState runState = QueueRunState::Running;
    std::size_t maxConcurrency = 2;
    std::vector<JobRecord> records;
    // Ids in user-visible queue order. Rebuilt into SchedulerCore's pending order on load;
    // ids that no longer resolve to a pending record are dropped.
    std::vector<jobs::JobId> pendingOrder;

    nlohmann::json ToJson() const;
};

// How a load attempt went. Always usable -- `queue` is simply empty when the file was bad.
struct LoadOutcome {
    enum class Status {
        Loaded,        // a valid state file was read
        NotPresent,    // no state file yet; a normal first run
        Recovered,     // the file was unusable and was quarantined; starting empty
    };

    Status status = Status::NotPresent;
    PersistedQueue queue;
    // Human-readable explanation, logged by the caller. Empty on a clean load.
    std::string diagnostic;
    // Where the unusable file was moved, when status is Recovered and the move succeeded.
    std::optional<std::string> quarantinedPath;
};

class QueuePersistence {
public:
    // `stateFilePath` is created (with its parent directories) on the first Save().
    explicit QueuePersistence(std::string stateFilePath);

    const std::string& StateFilePath() const { return stateFilePath_; }

    // Atomically replaces the state file. Throws errors::MediaToolException only if the
    // write genuinely could not be completed -- callers treat that as non-fatal and keep
    // running with an in-memory queue.
    void Save(const PersistedQueue& queue) const;

    // Never throws. See the file comment for the corruption policy.
    LoadOutcome Load() const;

    // Default location: alongside the app's other local state.
    static std::string DefaultStateFilePath();

private:
    // Moves an unusable state file to "<name>.corrupt-<timestamp>" so the evidence survives
    // for diagnosis instead of being silently overwritten.
    std::optional<std::string> Quarantine() const;

    std::string stateFilePath_;
};

// Applies restart recovery to freshly loaded records (spec section 25).
//
// Policy, chosen deliberately: a job that was executing when the process died becomes
// FAILED with code E_JOB_INTERRUPTED, not QUEUED.
//
// The reasoning is that we cannot tell what state its output is in. A half-written download
// or a killed ffmpeg encode leaves bytes on disk that may or may not be usable, and neither
// yt-dlp nor ffmpeg is being asked to resume here. Silently re-queueing would risk
// re-downloading gigabytes without the user asking, or worse, treating a truncated file as
// a valid source for the next job in a pipeline. Failing loudly with a retryable error puts
// the decision where it belongs -- and E_JOB_INTERRUPTED is classified transient, so a
// user-initiated retry (or an automatic one, if the job had budget) starts cleanly from
// scratch after the job's own artifact sweep.
//
// Returns the ids that were recovered, for logging.
std::vector<jobs::JobId> ApplyRestartRecovery(std::vector<JobRecord>& records);

}  // namespace mediatool::queue
