#pragma once

// File inspection result + capability vocabulary (spec sections 14-15). FileInspector
// implementations populate as many fields as they can; media-specific fields are
// std::nullopt when the file isn't media or ffprobe isn't available.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mediatool::filesystem {

enum class FileCategory {
    Video,
    Audio,
    Image,
    Document,
    Text,
    Archive,
    Unknown,
};

std::string ToWireString(FileCategory category);
FileCategory FileCategoryFromWireString(const std::string& wire);

struct FileInfo {
    std::string path;
    std::string filename;
    std::string extension;       // without leading dot, lowercase
    FileCategory category = FileCategory::Unknown;
    std::uint64_t sizeBytes = 0;
    std::optional<std::string> mimeType;

    // Media-specific, populated via ffprobe when available (spec section 14).
    std::optional<double> durationSeconds;
    std::optional<int> width;
    std::optional<int> height;
    std::optional<std::string> videoCodec;
    std::optional<std::string> audioCodec;
    std::optional<std::int64_t> bitrate;
    std::optional<double> fps;

    nlohmann::json ToJson() const;
    static FileInfo FromJson(const nlohmann::json& json);
};

// Capability tokens exposed per file category (spec section 15), e.g. "compress",
// "convert", "extractAudio", "extractFrames", "resize", "convertToText", "convertToHtml".
// Kept as free-form strings rather than an enum: the set grows every phase and the
// frontend only ever needs to check membership, never branch on a fixed enum.
std::vector<std::string> CapabilitiesFor(FileCategory category, const std::string& extension);

}  // namespace mediatool::filesystem
