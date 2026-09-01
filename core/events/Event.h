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
    // A failed ATTEMPT that is about to be repeated -- not a terminal outcome. Carries
    // the attempt count, the limit, the backoff, and the error that triggered it.
    JobRetrying,
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
    nlohmann::json ToJson() const;
};

// Constructs an Event with `timestampUtc` filled in from the system clock.
Event MakeEvent(EventType type, nlohmann::json data, std::optional<std::string> jobId = std::nullopt);

}  // namespace mediatool::events
