#include "core/queue/JobRecord.h"

#include "core/errors/MediaToolException.h"

namespace mediatool::queue {

namespace {

using errors::ErrorCategory;
using errors::ErrorInfo;
using errors::MediaToolException;

// Reads `key` as T, falling back to `fallback` when it is absent, null, or the wrong JSON
// type. Every optional field in a persisted record goes through here: a state file written
// by an older build, or truncated mid-write, must load with sensible defaults instead of
// throwing (spec section 24).
template <typename T>
T Get(const nlohmann::json& json, const char* key, T fallback) {
    if (!json.contains(key) || json[key].is_null()) return fallback;
    try {
        return json[key].get<T>();
    } catch (const std::exception&) {
        return fallback;
    }
}

}  // namespace

nlohmann::json JobSpec::ToJson() const {
    return {{"type", jobs::ToWireString(type)}, {"params", params}};
}

JobSpec JobSpec::FromJson(const nlohmann::json& json) {
    JobSpec spec;
    if (!json.is_object()) return spec;
    if (json.contains("type") && json["type"].is_string()) {
        try {
            spec.type = jobs::JobTypeFromWireString(json["type"].get<std::string>());
        } catch (const std::invalid_argument&) {
            // An unknown job type from a newer build. Left at the default; the caller
            // decides what to do with a record it cannot rebuild (see QueuePersistence).
            spec.type = jobs::JobType::Test;
        }
    }
    if (json.contains("params") && json["params"].is_object()) spec.params = json["params"];
    return spec;
}

nlohmann::json JobRecord::ToJson() const {
    nlohmann::json json;
    json["id"] = id;
    json["spec"] = spec.ToJson();
    json["state"] = jobs::ToWireString(state);
    json["priority"] = ToWireString(priority);
    json["sequence"] = sequence;
    json["createdAtMs"] = createdAtMs;
    json["pendingSinceMs"] = pendingSinceMs;
    if (finishedAtMs) json["finishedAtMs"] = *finishedAtMs;
    json["attempt"] = attempt;
    json["retryPolicy"] = retryPolicy.ToJson();
    if (nextRetryAtMs) json["nextRetryAtMs"] = *nextRetryAtMs;
    if (!lastRetryReason.empty()) json["lastRetryReason"] = lastRetryReason;
    json["dependencies"] = dependencies;
    if (parentJobId) json["parentJobId"] = *parentJobId;
    if (!duplicateKey.empty()) json["duplicateKey"] = duplicateKey;
    json["metadata"] = metadata;
    json["revision"] = revision;
    return json;
}

JobRecord JobRecord::FromJson(const nlohmann::json& json) {
    if (!json.is_object() || !json.contains("id") || !json["id"].is_string() ||
        json["id"].get<std::string>().empty()) {
        throw MediaToolException(ErrorInfo::Make("E_QUEUE_RECORD_INVALID", ErrorCategory::InvalidFile,
                                                  "A persisted queue entry has no usable id."));
    }

    JobRecord record;
    record.id = json["id"].get<std::string>();
    if (json.contains("spec")) record.spec = JobSpec::FromJson(json["spec"]);

    // An unrecognized enum value must not take the whole state file down with it -- fall
    // back to a safe value and let restart recovery sort it out (spec section 24).
    if (json.contains("state") && json["state"].is_string()) {
        try {
            record.state = jobs::JobStateFromWireString(json["state"].get<std::string>());
        } catch (const std::invalid_argument&) {
            record.state = jobs::JobState::Failed;
        }
    }
    if (json.contains("priority") && json["priority"].is_string()) {
        try {
            record.priority = JobPriorityFromWireString(json["priority"].get<std::string>());
        } catch (const MediaToolException&) {
            record.priority = JobPriority::Normal;
        }
    }

    record.sequence = Get<std::int64_t>(json, "sequence", 0);
    record.createdAtMs = Get<std::int64_t>(json, "createdAtMs", 0);
    record.pendingSinceMs = Get<std::int64_t>(json, "pendingSinceMs", record.createdAtMs);
    if (json.contains("finishedAtMs") && json["finishedAtMs"].is_number_integer())
        record.finishedAtMs = json["finishedAtMs"].get<std::int64_t>();
    record.attempt = Get<int>(json, "attempt", 0);

    if (json.contains("retryPolicy")) {
        try {
            record.retryPolicy = RetryPolicy::FromJson(json["retryPolicy"]);
        } catch (const MediaToolException&) {
            record.retryPolicy = RetryPolicy{};  // out-of-range values -> defaults
        }
    }
    if (json.contains("nextRetryAtMs") && json["nextRetryAtMs"].is_number_integer())
        record.nextRetryAtMs = json["nextRetryAtMs"].get<std::int64_t>();
    record.lastRetryReason = Get<std::string>(json, "lastRetryReason", "");

    if (json.contains("dependencies") && json["dependencies"].is_array()) {
        for (const auto& dep : json["dependencies"]) {
            if (dep.is_string() && !dep.get<std::string>().empty())
                record.dependencies.push_back(dep.get<std::string>());
        }
    }
    if (json.contains("parentJobId") && json["parentJobId"].is_string())
        record.parentJobId = json["parentJobId"].get<std::string>();
    record.duplicateKey = Get<std::string>(json, "duplicateKey", "");
    if (json.contains("metadata") && json["metadata"].is_object()) record.metadata = json["metadata"];
    record.revision = Get<std::int64_t>(json, "revision", 0);
    return record;
}

}  // namespace mediatool::queue
