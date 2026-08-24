#include "core/media/ProcessingOptions.h"

#include <stdexcept>
#include <unordered_map>

#include "core/errors/MediaToolException.h"

namespace mediatool::media {

namespace {

using errors::ErrorCategory;
using errors::ErrorInfo;
using errors::MediaToolException;

[[noreturn]] void ThrowUnsupported(const std::string& what, const std::string& value) {
    throw MediaToolException(ErrorInfo::Make(
        "E_UNSUPPORTED_" + what, ErrorCategory::UnsupportedFormat,
        "Unsupported " + what + ": " + value,
        "no argument recipe exists for this value; see core/media/ProcessingOptions.h"));
}

// A caller-supplied integer that must land inside a sane range. Out-of-range values are a
// permanent input error, not something to clamp silently -- clamping would produce output
// the user did not ask for (spec section 54).
int RequireInRange(const nlohmann::json& json, const char* key, int low, int high) {
    if (!json.at(key).is_number_integer()) {
        throw MediaToolException(ErrorInfo::Make(
            "E_INVALID_PROCESSING_OPTION", ErrorCategory::UnsupportedFormat,
            std::string(key) + " must be a whole number"));
    }
    const int value = json.at(key).get<int>();
    if (value < low || value > high) {
        throw MediaToolException(ErrorInfo::Make(
            "E_INVALID_PROCESSING_OPTION", ErrorCategory::UnsupportedFormat,
            std::string(key) + " must be between " + std::to_string(low) + " and " +
                std::to_string(high),
            key + std::string("=") + std::to_string(value)));
    }
    return value;
}

}  // namespace

std::string ToWireString(TargetFormat format) {
    switch (format) {
        case TargetFormat::Mp4: return "MP4";
        case TargetFormat::Mkv: return "MKV";
        case TargetFormat::WebM: return "WEBM";
        case TargetFormat::Mov: return "MOV";
        case TargetFormat::Gif: return "GIF";
        case TargetFormat::Mp3: return "MP3";
        case TargetFormat::Wav: return "WAV";
        case TargetFormat::M4a: return "M4A";
        case TargetFormat::Flac: return "FLAC";
        case TargetFormat::Opus: return "OPUS";
    }
    return "MP4";
}

TargetFormat TargetFormatFromWireString(const std::string& wire) {
    static const std::unordered_map<std::string, TargetFormat> kTable{
        {"MP4", TargetFormat::Mp4},   {"MKV", TargetFormat::Mkv},
        {"WEBM", TargetFormat::WebM}, {"MOV", TargetFormat::Mov},
        {"GIF", TargetFormat::Gif},   {"MP3", TargetFormat::Mp3},
        {"WAV", TargetFormat::Wav},   {"M4A", TargetFormat::M4a},
        {"FLAC", TargetFormat::Flac}, {"OPUS", TargetFormat::Opus},
    };
    const auto it = kTable.find(wire);
    if (it == kTable.end()) ThrowUnsupported("TARGET_FORMAT", wire);
    return it->second;
}

std::string ExtensionFor(TargetFormat format) {
    switch (format) {
        case TargetFormat::Mp4: return "mp4";
        case TargetFormat::Mkv: return "mkv";
        case TargetFormat::WebM: return "webm";
        case TargetFormat::Mov: return "mov";
        case TargetFormat::Gif: return "gif";
        case TargetFormat::Mp3: return "mp3";
        case TargetFormat::Wav: return "wav";
        case TargetFormat::M4a: return "m4a";
        case TargetFormat::Flac: return "flac";
        case TargetFormat::Opus: return "opus";
    }
    return "mp4";
}

bool IsAudioOnly(TargetFormat format) {
    switch (format) {
        case TargetFormat::Mp3:
        case TargetFormat::Wav:
        case TargetFormat::M4a:
        case TargetFormat::Flac:
        case TargetFormat::Opus:
            return true;
        case TargetFormat::Mp4:
        case TargetFormat::Mkv:
        case TargetFormat::WebM:
        case TargetFormat::Mov:
        case TargetFormat::Gif:
            return false;
    }
    return false;
}

std::vector<std::string> AllTargetFormatWireStrings() {
    return {"MP4", "MKV", "WEBM", "MOV", "GIF", "MP3", "WAV", "M4A", "FLAC", "OPUS"};
}

std::string ToWireString(CompressionPreset preset) {
    switch (preset) {
        case CompressionPreset::Low: return "LOW";
        case CompressionPreset::Medium: return "MEDIUM";
        case CompressionPreset::High: return "HIGH";
    }
    return "MEDIUM";
}

CompressionPreset CompressionPresetFromWireString(const std::string& wire) {
    if (wire == "LOW") return CompressionPreset::Low;
    if (wire == "MEDIUM") return CompressionPreset::Medium;
    if (wire == "HIGH") return CompressionPreset::High;
    ThrowUnsupported("COMPRESSION_PRESET", wire);
}

nlohmann::json ConversionRequest::ToJson() const {
    nlohmann::json json;
    json["targetFormat"] = ToWireString(targetFormat);
    if (audioBitrateKbps) json["audioBitrateKbps"] = *audioBitrateKbps;
    if (gifFps) json["gifFps"] = *gifFps;
    if (maxHeight) json["maxHeight"] = *maxHeight;
    return json;
}

ConversionRequest ConversionRequest::FromJson(const nlohmann::json& json) {
    ConversionRequest request;
    if (!json.is_object()) {
        throw MediaToolException(ErrorInfo::Make("E_INVALID_PROCESSING_OPTION",
                                                  ErrorCategory::UnsupportedFormat,
                                                  "Conversion options must be an object"));
    }
    if (!json.contains("targetFormat")) {
        throw MediaToolException(ErrorInfo::Make("E_INVALID_PROCESSING_OPTION",
                                                  ErrorCategory::UnsupportedFormat,
                                                  "A target format is required."));
    }
    if (!json.at("targetFormat").is_string()) {
        throw MediaToolException(ErrorInfo::Make("E_INVALID_PROCESSING_OPTION",
                                                  ErrorCategory::UnsupportedFormat,
                                                  "targetFormat must be a string"));
    }
    request.targetFormat = TargetFormatFromWireString(json.at("targetFormat").get<std::string>());
    if (json.contains("audioBitrateKbps"))
        request.audioBitrateKbps = RequireInRange(json, "audioBitrateKbps", 8, 2048);
    if (json.contains("gifFps")) request.gifFps = RequireInRange(json, "gifFps", 1, 60);
    if (json.contains("maxHeight")) request.maxHeight = RequireInRange(json, "maxHeight", 16, 8192);
    return request;
}

nlohmann::json CompressionRequest::ToJson() const {
    nlohmann::json json;
    json["preset"] = ToWireString(preset);
    if (maxHeight) json["maxHeight"] = *maxHeight;
    if (audioBitrateKbps) json["audioBitrateKbps"] = *audioBitrateKbps;
    return json;
}

CompressionRequest CompressionRequest::FromJson(const nlohmann::json& json) {
    CompressionRequest request;
    if (!json.is_object()) {
        throw MediaToolException(ErrorInfo::Make("E_INVALID_PROCESSING_OPTION",
                                                  ErrorCategory::UnsupportedFormat,
                                                  "Compression options must be an object"));
    }
    if (json.contains("preset")) {
        if (!json.at("preset").is_string()) {
            throw MediaToolException(ErrorInfo::Make("E_INVALID_PROCESSING_OPTION",
                                                      ErrorCategory::UnsupportedFormat,
                                                      "preset must be a string"));
        }
        request.preset = CompressionPresetFromWireString(json.at("preset").get<std::string>());
    }
    if (json.contains("maxHeight")) request.maxHeight = RequireInRange(json, "maxHeight", 16, 8192);
    if (json.contains("audioBitrateKbps"))
        request.audioBitrateKbps = RequireInRange(json, "audioBitrateKbps", 8, 2048);
    return request;
}

}  // namespace mediatool::media
