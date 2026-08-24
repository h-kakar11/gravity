#pragma once

// Structured event system (spec section 8, docs/ipc-contract.md). Events are never
// human-readable log lines the frontend must parse -- they carry typed data as JSON.
//
// EventType enum names use PascalCase to match spec section 8; ToWireString() converts
// to the camelCase string that actually crosses the IPC boundary (e.g.
// EventType::JobCreated -> "jobCreated"). Always go through ToWireString/Serialize --
// never hand-format the wire string.

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace mediatool::events {

enum class EventType {
    JobCreated,
    JobQueued,
    JobStarted,
    JobProgress,
    JobPaused,
    JobResumed,
    JobCompleted,
    JobFailed,
    JobCancelled,
    // A dependency failed or was cancelled, so this job will never run (Phase 5).
    JobSkipped,
    // An automatic retry has been scheduled; data carries attempt/delayMs/reason.
    JobRetryScheduled,
    // Queue-level change: pause/resume, concurrency, ordering, or aggregate counts. Carries
    // the queue's run state, concurrency and statistics so a listener does not have to
    // round-trip getQueueSnapshot for the common case.
    QueueChanged,
    FileDetected,
    HardwareDetected,
    DownloadMetadataReceived,
    LogEvent,
};

std::string ToWireString(EventType type);

struct Event {
    EventType type;
    std::optional<std::string> jobId;  // present for all Job* events
    nlohmann::json data;               // event-specific payload, see docs/ipc-contract.md
    std::string timestampUtc;          // ISO-8601, set by MakeEvent()

    // Serializes to the exact NDJSON line shape from docs/ipc-contract.md:
    // {"event": "...", "jobId"?: "...", "timestamp": "...", "data": {...}}
    //
    // Note the absence of a sequence number here. Ordering is a property of the stdout
    // channel, not of the event itself, so the `seq` field is stamped by whatever writes
    // the line -- under the same lock that serializes writes, which is what makes wire
    // order and sequence order the same thing (see app/core/main.cpp).
    nlohmann::json ToJson() const;
};

// Constructs an Event with `timestampUtc` filled in from the system clock.
Event MakeEvent(EventType type, nlohmann::json data, std::optional<std::string> jobId = std::nullopt);

}  // namespace mediatool::events
