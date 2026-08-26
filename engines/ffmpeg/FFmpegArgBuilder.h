#pragma once

// Pure argv construction for FFmpegEngine::Convert/Compress (spec section 16, Phase 2).
// Deliberately separated from FFmpegEngine itself so the argument-construction logic --
// which quality tier maps to which CRF, which encoder a codec+hw-accel combination
// resolves to, how trim/watermark/resolution/gif compose into one filter graph -- is
// fully unit-testable without a real ffmpeg binary or IProcessRunner.
//
// Compress is not a structurally different invocation from Convert: both jobs (see
// core/jobs/MediaProcessingJob.h) call BuildFfmpegArgs with the same options shape, just
// different default option values (Compress defaults to a smaller/lower-bitrate preset).

#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mediatool::media {

struct WatermarkOptions {
    std::string imagePath;
    // "top-left" | "top-right" | "bottom-left" | "bottom-right" | "center"
    std::string position = "bottom-right";
    double opacity = 1.0;  // 0.0-1.0
};

struct MediaProcessingOptions {
    std::string outputFormat;  // "mp4" | "webm" | "mov" | "gif" | "mp3" | "wav" | ... (required)

    // "low" | "medium" | "high" | "lossless". "lossless" is Pro-gated and rejected before
    // a job is ever created (see main.cpp's HandleCreateConversionJob/HandleCreateCompressionJob)
    // -- BuildFfmpegArgs itself still handles it (CRF 0) rather than assuming the caller
    // already filtered it, so it stays independently correct/testable.
    std::string quality = "medium";

    std::string videoCodec = "auto";            // "auto" | "h264" | "h265" | "vp9" | "av1"
    std::string hardwareAcceleration = "auto";  // "auto" | "none" | "nvenc" | "amf" | "qsv"

    std::optional<int> resolutionWidth;
    std::optional<int> resolutionHeight;

    std::optional<double> trimStartSeconds;
    std::optional<double> trimEndSeconds;

    std::optional<WatermarkOptions> watermark;

    std::optional<int> audioBitrateKbps;

    static MediaProcessingOptions FromJson(const nlohmann::json& json);
};

// `availableEncoders` is FFmpegEngine's cached DiscoverAvailableEncoders() result (the
// set of encoder names the resolved ffmpeg binary actually reports via `ffmpeg
// -encoders`). An empty set means "not probed" and is treated as "assume only the
// bundled default (libopenh264) is available" -- see the licensing-driven fallback logic
// in the .cpp file. Throws errors::MediaToolException{Unknown, "E_INVALID_MEDIA_OPTIONS",
// ...} for a malformed/missing outputFormat; never throws for a codec/hw-accel
// combination it can't satisfy exactly -- it degrades to a working alternative instead
// (see the .cpp file's encoder-selection comments for exactly what falls back to what).
std::vector<std::string> BuildFfmpegArgs(const std::string& inputPath, const std::string& outputPath,
                                          const MediaProcessingOptions& options,
                                          const std::set<std::string>& availableEncoders);

}  // namespace mediatool::media
