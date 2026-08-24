#pragma once

// Everything the scheduler knows about a job that is NOT inside the Job object itself.
//
// The split matters: a Job knows how to execute itself and what state it is in. It does
// not know its priority, its place in a queue, how many attempts it has had, or what it
// depends on -- those are scheduling concerns, and putting them on Job would make every
// job type carry queue machinery it has no use for. A JobRecord is also the unit that gets
// persisted, which is why it holds a `spec` sufficient to rebuild the Job from scratch
// after a restart (spec section 22) and deliberately holds no process handles, no PIDs,
// and no pointer to the live Job (spec section 22: "Do not persist volatile process
// handles").

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/jobs/JobTypes.h"
#include "core/queue/QueueTypes.h"

namespace mediatool::queue {

// The recipe for (re)building a runnable Job. `params` is the same validated object the
// createJob IPC command carried, so a restored job is constructed exactly the way the
// original request would have constructed it.
struct JobSpec {
    jobs::JobType type = jobs::JobType::Test;
    nlohmann::json params = nlohmann::json::object();

    nlohmann::json ToJson() const;
    static JobSpec FromJson(const nlohmann::json& json);
};

struct JobRecord {
    jobs::JobId id;
    JobSpec spec;
    // Mirrors the live Job's state. JobManager is responsible for keeping the two in step;
    // SchedulerCore never reads a Job directly, so this is its only view of progress.
    jobs::JobState state = jobs::JobState::Queued;
    JobPriority priority = JobPriority::Normal;

    // Monotonic creation counter. Immutable, unique, and the last-resort ordering
    // tie-break -- unlike a timestamp it cannot collide when two jobs are created inside
    // the same millisecond.
    std::int64_t sequence = 0;

    std::int64_t createdAtMs = 0;
    // When this job most recently entered the pending set. Fairness aging measures from
    // here, so a job that ran, failed, and is now retrying ages from the retry, not from
    // its original creation.
    std::int64_t pendingSinceMs = 0;
    std::optional<std::int64_t> finishedAtMs;

    // 0 while running for the first time; N once the job is on its Nth retry.
    int attempt = 0;
    RetryPolicy retryPolicy;
    // Only meaningful in RetryWait: the wall-clock instant the next attempt becomes
    // eligible.
    std::optional<std::int64_t> nextRetryAtMs;
    // Why the last automatic retry decision went the way it did, for the detail panel.
    std::string lastRetryReason;

    std::vector<jobs::JobId> dependencies;
    std::optional<jobs::JobId> parentJobId;

    // Identity for duplicate detection. Empty disables duplicate checking for this job.
    std::string duplicateKey;

    // Free-form descriptive fields for the UI (title, filenames, preset, ...). Distinct
    // from the Job's own metadata, which the Job fills in as it learns things; this copy is
    // what survives a restart.
    nlohmann::json metadata = nlohmann::json::object();

    // Bumped on every durable change. The frontend uses it to discard events that arrive
    // out of order (spec section 57).
    std::int64_t revision = 0;

    nlohmann::json ToJson() const;
    // Tolerant of missing/unknown fields: an older or partially-written state file must
    // load rather than crash (spec section 24). Throws only if `id` is unusable.
    static JobRecord FromJson(const nlohmann::json& json);
};

}  // namespace mediatool::queue
