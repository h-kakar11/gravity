#pragma once

// Structured options for the two real media-processing operations: conversion (change
// container/codec) and compression (re-encode smaller at the same container).
//
// IMediaEngine::Convert/Compress take an opaque `nlohmann::json options` bag -- that
// signature predates this header and is deliberately left alone. These structs are the
// typed form that both sides agree on: jobs build one and call ToJson(); FFmpegEngine
// calls FromJson() and hands the result to FFmpegArgumentBuilder. Nothing anywhere should
// hand-assemble the option keys as raw JSON.
//
// FromJson() throws errors::MediaToolException (ErrorCategory::UnsupportedFormat) on an
// unknown format/preset rather than silently falling back to a default -- an invalid
// preset is a permanent, non-retryable error (see core/queue/RetryClassifier.h).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mediatool::media {

// The container/format a conversion targets. Deliberately a closed set: every entry here
// has a verified argv recipe in FFmpegArgumentBuilder, and offering a format we have not
// actually built arguments for is exactly the "superficial placeholder" this codebase
// avoids. Extend both together.
enum class TargetFormat {
    // Video containers
    Mp4,
    Mkv,
    WebM,
    Mov,
    Gif,
    // Audio-only containers
    Mp3,
    Wav,
    M4a,
    Flac,
    Opus,
};

std::string ToWireString(TargetFormat format);
TargetFormat TargetFormatFromWireString(const std::string& wire);

// The file extension (no leading dot) a TargetFormat produces.
std::string ExtensionFor(TargetFormat format);

// True if the format carries no video stream, i.e. conversion to it always drops video.
bool IsAudioOnly(TargetFormat format);

// Every format a caller may pass, in wire form -- used by the IPC layer to report
// capabilities and by tests to assert the closed set stays in sync.
std::vector<std::string> AllTargetFormatWireStrings();

// Quality tiers for compression. Maps to a CRF (constant rate factor) in the argument
// builder; lower CRF = higher quality = bigger file.
enum class CompressionPreset {
    Low,     // smallest file, most visible quality loss
    Medium,  // the default
    High,    // near-transparent, modest saving
};

std::string ToWireString(CompressionPreset preset);
CompressionPreset CompressionPresetFromWireString(const std::string& wire);

struct ConversionRequest {
    TargetFormat targetFormat = TargetFormat::Mp4;
    // Audio bitrate for lossy audio targets. Ignored for Wav/Flac (lossless) and Gif.
    std::optional<int> audioBitrateKbps;
    // Frames per second for Gif output. Ignored for every other target.
    std::optional<int> gifFps;
    // Cap the output's long-edge height, preserving aspect ratio. nullopt = keep source.
    std::optional<int> maxHeight;

    nlohmann::json ToJson() const;
    static ConversionRequest FromJson(const nlohmann::json& json);
};

struct CompressionRequest {
    CompressionPreset preset = CompressionPreset::Medium;
    // Cap the output's height, preserving aspect ratio. nullopt = keep source resolution.
    std::optional<int> maxHeight;
    // Audio bitrate of the re-encoded audio track.
    std::optional<int> audioBitrateKbps;

    nlohmann::json ToJson() const;
    static CompressionRequest FromJson(const nlohmann::json& json);
};

}  // namespace mediatool::media
