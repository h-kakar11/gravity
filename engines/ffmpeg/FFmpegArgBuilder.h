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

#include "core/media/BitrateTarget.h"

namespace mediatool::media {

struct WatermarkOptions {
    std::string imagePath;
    // "top-left" | "top-right" | "bottom-left" | "bottom-right" | "center"
    std::string position = "bottom-right";
    double opacity = 1.0;  // 0.0-1.0
};

struct MediaProcessingOptions {
    std::string outputFormat;  // "mp4" | "webm" | "mov" | "gif" | "mp3" | "wav" | ... (required)

    // "lowest" | "low" | "medium" | "high" | "ultra" | "lossless" -- the same five-tier
    // vocabulary core/settings/Settings.h uses for processing.defaultCompressionQuality
    // (issue #59), plus "lossless". Issue #83: the Convert/Compress page previously offered
    // only low/medium/high, so the two extremes Settings advertised were unreachable from
    // the page that actually runs a job; issue #82 removed the Pro tier, so "lossless" is
    // now an ordinary selectable value rather than a server-rejected one.
    std::string quality = "medium";

    std::string videoCodec = "auto";            // "auto" | "h264" | "h265" | "vp9" | "av1"
    std::string hardwareAcceleration = "auto";  // "auto" | "none" | "nvenc" | "amf" | "qsv"

    std::optional<int> resolutionWidth;
    std::optional<int> resolutionHeight;

    std::optional<double> trimStartSeconds;
    std::optional<double> trimEndSeconds;

    std::optional<WatermarkOptions> watermark;

    std::optional<int> audioBitrateKbps;

    // An explicit video rate-control target, in kbps. When set, BuildFfmpegArgs emits
    // bitrate-based rate control (-b:v/-maxrate/-bufsize) INSTEAD of -crf.
    //
    // This exists because CRF is a *quality* target, not a *size* target: re-encoding an
    // already-compressed file at a fixed CRF routinely produces a LARGER file than the
    // source (issue #80 -- measured: a 1.19 MB 640x360 H.264 clip re-encoded at the
    // "medium" CRF 23 came back 1.03x its original size, and at "high" CRF 18, 1.41x).
    // A compression job therefore has to derive a target from the SOURCE bitrate, which
    // only the caller can know -- MediaProcessingJob probes the input and fills this in.
    //
    // It is also the only quality lever that reaches libopenh264, Gravity's default
    // (licensing-driven) software H.264 encoder: libopenh264 defines no `crf` AVOption at
    // all, so ffmpeg silently discards -crf for it -- exit code 0, and the "has not been
    // used for any stream" warning suppressed by this file's own `-loglevel error`. That
    // is why issue #80 saw byte-identical output at BOTH high and low quality: every
    // encode fell through to libopenh264's hardcoded 2 Mbps TARGET_BITRATE_DEFAULT.
    std::optional<int> videoBitrateKbps;

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
