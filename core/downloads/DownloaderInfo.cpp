#include "core/downloads/IDownloadProvider.h"

namespace mediatool::downloads {

nlohmann::json DownloaderInfo::ToJson() const {
    nlohmann::json json;
    json["available"] = available;
    json["backend"] = backend;
    json["stale"] = stale;
    // Null rather than omitted: the frontend renders "unknown" for a downloader it could
    // not identify, and an absent key and a null one should not have to mean two things.
    json["version"] = version.has_value() ? nlohmann::json(*version) : nlohmann::json();
    json["ageDays"] = ageDays.has_value() ? nlohmann::json(*ageDays) : nlohmann::json();
    return json;
}

}  // namespace mediatool::downloads
