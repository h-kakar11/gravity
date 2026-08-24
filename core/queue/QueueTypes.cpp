#include "core/queue/QueueTypes.h"

#include <stdexcept>

#include "core/errors/MediaToolException.h"

namespace mediatool::queue {

namespace {

using errors::ErrorCategory;
using errors::ErrorInfo;
using errors::MediaToolException;

[[noreturn]] void ThrowUnknown(const std::string& what, const std::string& wire) {
    throw MediaToolException(ErrorInfo::Make("E_INVALID_" + what, ErrorCategory::Unknown,
                                              "Unrecognized " + what + ": " + wire));
}

}  // namespace

std::string ToWireString(JobPriority priority) {
    switch (priority) {
        case JobPriority::Low: return "LOW";
        case JobPriority::Normal: return "NORMAL";
        case JobPriority::High: return "HIGH";
    }
    return "NORMAL";
}

JobPriority JobPriorityFromWireString(const std::string& wire) {
    if (wire == "LOW") return JobPriority::Low;
    if (wire == "NORMAL") return JobPriority::Normal;
    if (wire == "HIGH") return JobPriority::High;
    ThrowUnknown("PRIORITY", wire);
}

int PriorityRank(JobPriority priority) {
    // Spaced by 10 so fairness aging can lift a starved job past the tier above it in
    // bounded, whole steps without needing fractional ranks.
    switch (priority) {
        case JobPriority::Low: return 0;
        case JobPriority::Normal: return 10;
        case JobPriority::High: return 20;
    }
    return 10;
}

std::string ToWireString(MoveDirection direction) {
    switch (direction) {
        case MoveDirection::Top: return "TOP";
        case MoveDirection::Up: return "UP";
        case MoveDirection::Down: return "DOWN";
        case MoveDirection::Bottom: return "BOTTOM";
    }
    return "TOP";
}

MoveDirection MoveDirectionFromWireString(const std::string& wire) {
    if (wire == "TOP") return MoveDirection::Top;
    if (wire == "UP") return MoveDirection::Up;
    if (wire == "DOWN") return MoveDirection::Down;
    if (wire == "BOTTOM") return MoveDirection::Bottom;
    ThrowUnknown("MOVE_DIRECTION", wire);
}

std::string ToWireString(QueueRunState state) {
    return state == QueueRunState::Paused ? "PAUSED" : "RUNNING";
}

QueueRunState QueueRunStateFromWireString(const std::string& wire) {
    if (wire == "RUNNING") return QueueRunState::Running;
    if (wire == "PAUSED") return QueueRunState::Paused;
    ThrowUnknown("QUEUE_RUN_STATE", wire);
}

std::string ToWireString(HistoryScope scope) {
    switch (scope) {
        case HistoryScope::Completed: return "COMPLETED";
        case HistoryScope::Failed: return "FAILED";
        case HistoryScope::Cancelled: return "CANCELLED";
        case HistoryScope::Skipped: return "SKIPPED";
        case HistoryScope::All: return "ALL";
    }
    return "ALL";
}

HistoryScope HistoryScopeFromWireString(const std::string& wire) {
    if (wire == "COMPLETED") return HistoryScope::Completed;
    if (wire == "FAILED") return HistoryScope::Failed;
    if (wire == "CANCELLED") return HistoryScope::Cancelled;
    if (wire == "SKIPPED") return HistoryScope::Skipped;
    if (wire == "ALL") return HistoryScope::All;
    ThrowUnknown("HISTORY_SCOPE", wire);
}

nlohmann::json RetryPolicy::ToJson() const {
    return {{"maxRetries", maxRetries},
            {"initialDelayMs", initialDelayMs},
            {"maxDelayMs", maxDelayMs},
            {"multiplier", multiplier}};
}

RetryPolicy RetryPolicy::FromJson(const nlohmann::json& json) {
    RetryPolicy policy;
    if (!json.is_object()) return policy;
    if (json.contains("maxRetries") && json["maxRetries"].is_number_integer())
        policy.maxRetries = json["maxRetries"].get<int>();
    if (json.contains("initialDelayMs") && json["initialDelayMs"].is_number_integer())
        policy.initialDelayMs = json["initialDelayMs"].get<std::int64_t>();
    if (json.contains("maxDelayMs") && json["maxDelayMs"].is_number_integer())
        policy.maxDelayMs = json["maxDelayMs"].get<std::int64_t>();
    if (json.contains("multiplier") && json["multiplier"].is_number())
        policy.multiplier = json["multiplier"].get<double>();
    policy.Validate();
    return policy;
}

void RetryPolicy::Validate() const {
    const auto fail = [](const std::string& detail) -> void {
        throw MediaToolException(ErrorInfo::Make("E_INVALID_RETRY_POLICY", ErrorCategory::Unknown,
                                                  "The retry settings are not valid.", detail));
    };
    // An unbounded retry count is exactly what spec section 13 forbids, so the ceiling is
    // enforced here rather than left to callers to remember.
    if (maxRetries < 0 || maxRetries > 20)
        fail("maxRetries must be between 0 and 20, got " + std::to_string(maxRetries));
    if (initialDelayMs < 0 || initialDelayMs > 3'600'000)
        fail("initialDelayMs must be between 0 and 3600000, got " + std::to_string(initialDelayMs));
    if (maxDelayMs < initialDelayMs || maxDelayMs > 3'600'000)
        fail("maxDelayMs must be >= initialDelayMs and <= 3600000, got " +
             std::to_string(maxDelayMs));
    if (!(multiplier >= 1.0) || multiplier > 10.0)
        fail("multiplier must be between 1.0 and 10.0");
}

nlohmann::json QueueStatistics::ToJson() const {
    return {{"running", running},   {"queued", queued},     {"waiting", waiting},
            {"retryWait", retryWait}, {"paused", paused},   {"completed", completed},
            {"failed", failed},     {"cancelled", cancelled}, {"skipped", skipped},
            {"total", total}};
}

std::string MakeDuplicateKey(jobs::JobType type, const nlohmann::json& params) {
    // nlohmann::json orders object keys deterministically (std::map), so dump() is stable
    // across two structurally identical param objects regardless of the order the caller
    // wrote them in. That is what makes this a usable identity rather than a hash of
    // whatever spelling the frontend happened to send.
    // error_handler_t::replace, not the strict default -- see the identical comment in
    // core/queue/QueuePersistence.cpp. This runs inside job creation, which the IPC
    // loop's own try/catch would actually reach (unlike the persistence-thread case), but
    // there's no reason to let a duplicate-key computation throw at all when it doesn't
    // have to.
    return jobs::ToWireString(type) + "|" + params.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace mediatool::queue
