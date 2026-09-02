#include "core/downloads/IDownloadProvider.h"

namespace mediatool::downloads {

nlohmann::json PlaylistInfo::ToJson() const {
    nlohmann::json json;
    json["title"] = title;
    json["uploader"] = uploader.has_value() ? nlohmann::json(*uploader) : nlohmann::json();
    json["webpageUrl"] = webpageUrl.has_value() ? nlohmann::json(*webpageUrl) : nlohmann::json();
    json["truncated"] = truncated;
    // `count` is redundant with entries.length for a JSON caller, but the wire contract
    // documents it (docs/ipc-contract.md) and it keeps the shape identical to the Python
    // payload this is parsed from.
    json["count"] = static_cast<int>(entries.size());

    nlohmann::json entryArray = nlohmann::json::array();
    for (const auto& entry : entries) {
        nlohmann::json e;
        e["index"] = entry.index;
        e["url"] = entry.url;
        e["title"] = entry.title;
        e["durationSeconds"] =
            entry.durationSeconds.has_value() ? nlohmann::json(*entry.durationSeconds) : nlohmann::json();
        entryArray.push_back(std::move(e));
    }
    json["entries"] = std::move(entryArray);
    return json;
}

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
