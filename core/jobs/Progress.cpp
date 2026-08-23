#include "core/jobs/Progress.h"

namespace mediatool::jobs {

nlohmann::json Progress::ToJson() const {
    nlohmann::json json;
    // Every field but statusMessage is optional on the wire -- omit absent fields
    // entirely rather than emitting `null` (docs/ipc-contract.md Progress shape).
    if (percentage) json["percentage"] = *percentage;
    if (processedBytes) json["processedBytes"] = *processedBytes;
    if (totalBytes) json["totalBytes"] = *totalBytes;
    if (speedBytesPerSecond) json["speedBytesPerSecond"] = *speedBytesPerSecond;
    if (etaSeconds) json["etaSeconds"] = *etaSeconds;
    if (currentItem) json["currentItem"] = *currentItem;
    json["statusMessage"] = statusMessage;
    return json;
}

Progress Progress::FromJson(const nlohmann::json& json) {
    Progress progress;
    if (json.contains("percentage") && !json["percentage"].is_null())
        progress.percentage = json["percentage"].get<double>();
    if (json.contains("processedBytes") && !json["processedBytes"].is_null())
        progress.processedBytes = json["processedBytes"].get<std::uint64_t>();
    if (json.contains("totalBytes") && !json["totalBytes"].is_null())
        progress.totalBytes = json["totalBytes"].get<std::uint64_t>();
    if (json.contains("speedBytesPerSecond") && !json["speedBytesPerSecond"].is_null())
        progress.speedBytesPerSecond = json["speedBytesPerSecond"].get<double>();
    if (json.contains("etaSeconds") && !json["etaSeconds"].is_null())
        progress.etaSeconds = json["etaSeconds"].get<double>();
    if (json.contains("currentItem") && !json["currentItem"].is_null())
        progress.currentItem = json["currentItem"].get<std::string>();
    if (json.contains("statusMessage") && !json["statusMessage"].is_null())
        progress.statusMessage = json["statusMessage"].get<std::string>();
    return progress;
}

}  // namespace mediatool::jobs
