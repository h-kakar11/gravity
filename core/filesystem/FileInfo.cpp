#include "core/filesystem/FileInfo.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "core/media/DeferredOperations.h"

namespace mediatool::filesystem {

namespace {

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// Reads an optional field: absent key or explicit JSON null both mean nullopt. Avoids
// relying on nlohmann's own std::optional support so behavior doesn't depend on the
// vendored version's feature set.
template <typename T>
std::optional<T> ReadOptional(const nlohmann::json& json, const std::string& key) {
    const auto it = json.find(key);
    if (it == json.end() || it->is_null()) {
        return std::nullopt;
    }
    return it->get<T>();
}

}  // namespace

std::string ToWireString(FileCategory category) {
    switch (category) {
        case FileCategory::Video:
            return "VIDEO";
        case FileCategory::Audio:
            return "AUDIO";
        case FileCategory::Image:
            return "IMAGE";
        case FileCategory::Document:
            return "DOCUMENT";
        case FileCategory::Text:
            return "TEXT";
        case FileCategory::Archive:
            return "ARCHIVE";
        case FileCategory::Unknown:
            return "UNKNOWN";
    }
    throw std::invalid_argument("Unhandled FileCategory value");
}

FileCategory FileCategoryFromWireString(const std::string& wire) {
    if (wire == "VIDEO") return FileCategory::Video;
    if (wire == "AUDIO") return FileCategory::Audio;
    if (wire == "IMAGE") return FileCategory::Image;
    if (wire == "DOCUMENT") return FileCategory::Document;
    if (wire == "TEXT") return FileCategory::Text;
    if (wire == "ARCHIVE") return FileCategory::Archive;
    if (wire == "UNKNOWN") return FileCategory::Unknown;
    throw std::invalid_argument("Unrecognized FileCategory wire string: " + wire);
}

nlohmann::json FileInfo::ToJson() const {
    nlohmann::json json{
        {"path", path},
        {"filename", filename},
        {"extension", extension},
        {"category", ToWireString(category)},
        {"sizeBytes", sizeBytes},
    };

    if (mimeType) json["mimeType"] = *mimeType;
    if (durationSeconds) json["durationSeconds"] = *durationSeconds;
    if (width) json["width"] = *width;
    if (height) json["height"] = *height;
    if (videoCodec) json["videoCodec"] = *videoCodec;
    if (audioCodec) json["audioCodec"] = *audioCodec;
    if (bitrate) json["bitrate"] = *bitrate;
    if (fps) json["fps"] = *fps;

    return json;
}

FileInfo FileInfo::FromJson(const nlohmann::json& json) {
    FileInfo info;
    info.path = json.at("path").get<std::string>();
    info.filename = json.at("filename").get<std::string>();
    info.extension = json.at("extension").get<std::string>();
    info.category = FileCategoryFromWireString(json.at("category").get<std::string>());
    info.sizeBytes = json.at("sizeBytes").get<std::uint64_t>();

    info.mimeType = ReadOptional<std::string>(json, "mimeType");
    info.durationSeconds = ReadOptional<double>(json, "durationSeconds");
    info.width = ReadOptional<int>(json, "width");
    info.height = ReadOptional<int>(json, "height");
    info.videoCodec = ReadOptional<std::string>(json, "videoCodec");
    info.audioCodec = ReadOptional<std::string>(json, "audioCodec");
    info.bitrate = ReadOptional<std::int64_t>(json, "bitrate");
    info.fps = ReadOptional<double>(json, "fps");

    return info;
}

std::vector<std::string> CapabilitiesFor(FileCategory category, const std::string& extension) {
    const std::string ext = ToLowerAscii(extension);

    switch (category) {
        case FileCategory::Video:
            // extractAudio/extractFrames deliberately absent -- see
            // DeferredCapabilitiesFor() below and core/media/DeferredOperations.h.
            return {"compress", "convert"};
        case FileCategory::Audio:
            return {"compress", "convert"};
        case FileCategory::Image:
            return {"compress", "convert", "resize"};
        case FileCategory::Document:
        case FileCategory::Text:
            // Phase 1 only wires up markup-ish text formats through the (stub) document
            // engine; binary document formats (pdf/docx/...) get no capabilities yet
            // rather than a token that would fail if exercised.
            if (ext == "md" || ext == "markdown") {
                return {"convertToText", "convertToHtml"};
            }
            if (ext == "html" || ext == "htm") {
                return {"convertToText"};
            }
            if (ext == "txt" || ext == "log" || ext == "csv") {
                return {"convertToHtml"};
            }
            return {};
        case FileCategory::Archive:
        case FileCategory::Unknown:
            return {};
    }
    return {};
}

nlohmann::json DeferredCapability::ToJson() const {
    return {{"capability", capability}, {"reason", reason}};
}

std::vector<DeferredCapability> DeferredCapabilitiesFor(FileCategory category,
                                                         const std::string& /*extension*/) {
    // Only video carries deferred operations today: extracting an audio track or still
    // frames is meaningless for the other categories, so reporting them there would be a
    // second kind of lie. The reasons come from the deferral table itself rather than
    // being restated here -- see core/media/DeferredOperations.h.
    if (category != FileCategory::Video) {
        return {};
    }

    std::vector<DeferredCapability> deferred;
    for (const std::string& operation : media::DeferredOperations()) {
        deferred.push_back({operation, media::DeferralReason(operation)});
    }
    return deferred;
}

}  // namespace mediatool::filesystem
