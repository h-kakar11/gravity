#include "core/downloads/NdjsonLineProtocol.h"

namespace mediatool::downloads {

std::optional<nlohmann::json> ParseNdjsonLine(const std::string& line) {
    if (line.empty()) {
        return std::nullopt;
    }
    try {
        nlohmann::json parsed = nlohmann::json::parse(line);
        if (!parsed.is_object()) {
            return std::nullopt;
        }
        return parsed;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

namespace {

std::string EventField(const nlohmann::json& parsedLine) {
    const auto it = parsedLine.find("event");
    if (it == parsedLine.end() || !it->is_string()) {
        return "";
    }
    return it->get<std::string>();
}

}  // namespace

DownloaderEventType GetDownloaderEventType(const nlohmann::json& parsedLine) {
    const std::string event = EventField(parsedLine);
    if (event == "metadata") return DownloaderEventType::Metadata;
    if (event == "progress") return DownloaderEventType::Progress;
    if (event == "completed") return DownloaderEventType::Completed;
    if (event == "error") return DownloaderEventType::Error;
    return DownloaderEventType::Unknown;
}

bool IsMetadataEvent(const nlohmann::json& parsedLine) {
    return GetDownloaderEventType(parsedLine) == DownloaderEventType::Metadata;
}

bool IsProgressEvent(const nlohmann::json& parsedLine) {
    return GetDownloaderEventType(parsedLine) == DownloaderEventType::Progress;
}

bool IsCompletedEvent(const nlohmann::json& parsedLine) {
    return GetDownloaderEventType(parsedLine) == DownloaderEventType::Completed;
}

bool IsErrorEvent(const nlohmann::json& parsedLine) {
    return GetDownloaderEventType(parsedLine) == DownloaderEventType::Error;
}

}  // namespace mediatool::downloads
