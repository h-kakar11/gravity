#include "core/events/Event.h"

#include "core/common/IClock.h"

namespace mediatool::events {

std::string ToWireString(EventType type) {
    switch (type) {
        case EventType::JobCreated:
            return "jobCreated";
        case EventType::JobQueued:
            return "jobQueued";
        case EventType::JobStarted:
            return "jobStarted";
        case EventType::JobProgress:
            return "jobProgress";
        case EventType::JobPaused:
            return "jobPaused";
        case EventType::JobResumed:
            return "jobResumed";
        case EventType::JobCompleted:
            return "jobCompleted";
        case EventType::JobFailed:
            return "jobFailed";
        case EventType::JobCancelled:
            return "jobCancelled";
        case EventType::JobRetrying:
            return "jobRetrying";
        case EventType::FileDetected:
            return "fileDetected";
        case EventType::HardwareDetected:
            return "hardwareDetected";
        case EventType::DownloadMetadataReceived:
            return "downloadMetadataReceived";
        case EventType::LogEvent:
            return "logEvent";
    }
    // Unreachable for a valid enum value, but MinGW warns on missing return otherwise.
    return "";
}

nlohmann::json Event::ToJson() const {
    nlohmann::json json{
        {"event", ToWireString(type)},
        {"timestamp", timestampUtc},
        {"data", data},
    };
    // jobId is optional on the wire -- must be absent entirely when unset, never null,
    // per docs/ipc-contract.md.
    if (jobId.has_value()) {
        json["jobId"] = *jobId;
    }
    return json;
}

Event MakeEvent(EventType type, nlohmann::json data, std::optional<std::string> jobId) {
    static const common::SystemClock clock;
    Event event;
    event.type = type;
    event.jobId = std::move(jobId);
    event.data = std::move(data);
    event.timestampUtc = clock.NowIso8601Utc();
    return event;
}

}  // namespace mediatool::events
