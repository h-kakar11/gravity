#pragma once

// Pure parsing helper for the Python downloader NDJSON protocol (docs/ipc-contract.md,
// "Python downloader (downloader.py) <-> C++ core"). No process-launching here -- see
// engines/downloader/YtDlpProvider for that. Kept separate so it can be unit-tested with
// plain string literals.

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace mediatool::downloads {

// Parses one NDJSON line. Never throws: returns std::nullopt for malformed JSON or a
// non-object top-level value, since one bad line from a subprocess must not crash the
// core.
std::optional<nlohmann::json> ParseNdjsonLine(const std::string& line);

enum class DownloaderEventType {
    Metadata,
    Playlist,  // one `inspectPlaylist` result: the entry list to fan out into jobs
    Progress,
    Completed,
    Error,
    Unknown,  // forward-compat: an event name this build doesn't recognize yet
};

// `parsedLine` must be a JSON object, e.g. the result of ParseNdjsonLine.
DownloaderEventType GetDownloaderEventType(const nlohmann::json& parsedLine);

bool IsMetadataEvent(const nlohmann::json& parsedLine);
bool IsProgressEvent(const nlohmann::json& parsedLine);
bool IsCompletedEvent(const nlohmann::json& parsedLine);
bool IsErrorEvent(const nlohmann::json& parsedLine);

}  // namespace mediatool::downloads
