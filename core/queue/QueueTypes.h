#pragma once

// Vocabulary the queue orchestration layer adds on top of core/jobs (spec sections 5-9,
// 13-15, 20). Kept separate from JobTypes.h because these concepts belong to the
// *scheduler*, not to an individual Job: a Job knows how to execute itself and what state
// it is in, and knows nothing about priority, retry budgets, or its position in a queue.

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/jobs/JobTypes.h"

namespace mediatool::queue {

// Scheduling priority for *pending* work. Explicitly does not preempt: raising a queued
// job's priority reorders it, it never kills or restarts something already running
// (spec section 9).
enum class JobPriority {
    Low,
    Normal,
    High,
};

std::string ToWireString(JobPriority priority);
JobPriority JobPriorityFromWireString(const std::string& wire);

// Numeric rank used for ordering; higher wins. Also the base that fairness aging adds to,
// which is why it is spaced out rather than 0/1/2 -- see SchedulerCore::EffectivePriority.
int PriorityRank(JobPriority priority);

// Where a reorder request wants a pending job to end up (spec section 10).
enum class MoveDirection {
    Top,
    Up,
    Down,
    Bottom,
};

std::string ToWireString(MoveDirection direction);
MoveDirection MoveDirectionFromWireString(const std::string& wire);

// Whether the queue as a whole is admitting new work. Pausing never stops a job that is
// already running -- see docs/phase-5.md "Pause semantics".
enum class QueueRunState {
    Running,
    Paused,
};

std::string ToWireString(QueueRunState state);
QueueRunState QueueRunStateFromWireString(const std::string& wire);

// Bounded automatic-retry configuration (spec sections 13, 15).
struct RetryPolicy {
    // 0 disables automatic retries entirely for the job. Manual retry always remains
    // available regardless of this value.
    int maxRetries = 3;
    std::int64_t initialDelayMs = 2'000;
    std::int64_t maxDelayMs = 60'000;
    // Each attempt multiplies the previous delay by this factor.
    double multiplier = 2.0;

    nlohmann::json ToJson() const;
    static RetryPolicy FromJson(const nlohmann::json& json);

    // Throws errors::MediaToolException if any field is out of range. IPC input reaches
    // this struct, so it validates rather than trusting (spec section 54).
    void Validate() const;
};

// Which slice of finished history a clear operation removes (spec section 27). Never
// touches active or pending jobs, and never deletes files from disk.
enum class HistoryScope {
    Completed,
    Failed,
    Cancelled,
    Skipped,
    All,
};

std::string ToWireString(HistoryScope scope);
HistoryScope HistoryScopeFromWireString(const std::string& wire);

// Aggregate counts the frontend shows (spec section 33). Deliberately carries no overall
// percentage: a download measured in bytes and an encode measured in seconds have no
// meaningful common denominator, and averaging them would invent a number.
struct QueueStatistics {
    int running = 0;
    int queued = 0;
    int waiting = 0;
    int retryWait = 0;
    int paused = 0;
    int completed = 0;
    int failed = 0;
    int cancelled = 0;
    int skipped = 0;
    int total = 0;

    nlohmann::json ToJson() const;
};

// Identity used for duplicate detection (spec section 20). Two pending job requests are
// duplicates iff their keys are byte-identical; anything less exact is treated as a
// different request, because merging merely-similar jobs silently loses user intent.
std::string MakeDuplicateKey(jobs::JobType type, const nlohmann::json& params);

}  // namespace mediatool::queue
