#include "engines/ffmpeg/FFmpegArgBuilder.h"

#include <algorithm>
#include <sstream>
#include <unordered_set>

#include "core/errors/ErrorInfo.h"
#include "core/errors/MediaToolException.h"

namespace mediatool::media {

namespace {

using errors::ErrorCategory;
using errors::ErrorInfo;
using errors::MediaToolException;

[[noreturn]] void ThrowInvalidOptions(const std::string& reason) {
    throw MediaToolException(ErrorInfo::Make("E_INVALID_MEDIA_OPTIONS", ErrorCategory::Unknown,
                                               "Invalid conversion/compression options.", reason));
}

// The audio-only format set lives in core/media/BitrateTarget.h, because
// MediaProcessingJob needs exactly the same distinction when it sizes a compression job.
bool IsAudioOnlyFormat(const std::string& outputFormat) {
    return IsAudioOnlyOutputFormat(outputFormat);
}

// Static image targets -- explicitly NOT routed through the video (CRF/audio-encoder)
// path below: converting to e.g. .webp is a stated primary use case (idealist.md), and a
// single still frame has no bitrate/CRF video-quality story or audio stream to encode.
// GIF is handled by its own dedicated palette-pipeline branch, not here, despite also
// being an "image" format container-wise.
const std::unordered_set<std::string> kImageFormats = {
    "jpg", "jpeg", "png", "webp", "bmp", "tiff", "tif",
};

bool IsImageFormat(const std::string& outputFormat) { return kImageFormats.count(outputFormat) > 0; }

// Each image format has its own quality-control flag and scale; map the shared
// low/medium/high vocabulary onto whichever one applies. Formats with no meaningful
// quality knob (png is lossless by construction, bmp/tiff have none ffmpeg exposes here)
// simply get no extra flag.
std::vector<std::string> ImageQualityArgs(const std::string& outputFormat, const std::string& quality) {
    if (outputFormat == "webp") {
        // -quality 0-100, higher is better.
        int value = 75;
        if (quality == "lowest") value = 30;
        if (quality == "low") value = 50;
        if (quality == "high") value = 90;
        if (quality == "ultra" || quality == "lossless") value = 95;
        return {"-quality", std::to_string(value)};
    }
    if (outputFormat == "jpg" || outputFormat == "jpeg") {
        // -q:v (mjpeg quantizer) 2-31, LOWER is better -- inverted relative to every
        // other quality knob in this file, called out explicitly rather than left as a
        // trap for the next reader.
        int value = 5;
        if (quality == "lowest") value = 20;
        if (quality == "low") value = 12;
        if (quality == "high") value = 4;
        if (quality == "ultra" || quality == "lossless") value = 2;
        return {"-q:v", std::to_string(value)};
    }
    return {};
}

// CRF scales differ by codec family: h264/h265 use roughly 0-51 (visually lossless
// around 18, "fine" around 23, noticeably soft around 28); vp9/av1 use a wider 0-63
// range where the same subjective tiers land higher up.
int CrfForQuality(const std::string& quality, bool wideRange) {
    if (quality == "lossless") return 0;
    if (wideRange) {
        if (quality == "lowest") return 42;
        if (quality == "low") return 36;
        if (quality == "high") return 27;
        if (quality == "ultra") return 23;
        return 31;  // medium, and the default for any unrecognized value
    }
    if (quality == "lowest") return 34;
    if (quality == "low") return 28;
    if (quality == "high") return 20;
    if (quality == "ultra") return 17;
    return 23;  // medium, and the default for any unrecognized value
}

// Whether `encoder` actually implements ffmpeg's `-crf` private AVOption. This is not a
// stylistic preference -- passing -crf to an encoder that doesn't define it is a SILENT
// no-op: ffmpeg logs "Codec AVOption crf ... has not been used for any stream" at
// AV_LOG_WARNING (suppressed by the `-loglevel error` this builder emits) and exits 0.
// The encode then runs at that encoder's own default rate control, identically for every
// quality tier the user picks. That is issue #80's "same output size at both high and low
// quality", and it is the default path on a stock install: ResolveVideoEncoder() below
// falls back to libopenh264 (BSD-licensed, the only H.264 encoder Gravity can bundle),
// whose ffmpeg wrapper defines no `crf` option and defaults to a hardcoded 2 Mbps.
//
// Hardware encoders (NVENC/AMF/QSV) are excluded for the same reason plus their own: they
// use vendor-specific quality flags this product deliberately doesn't expose.
bool EncoderSupportsCrf(const std::string& encoder) {
    return encoder == "libx264" || encoder == "libx265" || encoder == "libvpx-vp9" ||
           encoder == "libaom-av1";
}

// Resolves videoCodec+hardwareAcceleration+availableEncoders down to one concrete ffmpeg
// encoder name. This is the licensing-load-bearing decision (docs/licensing.md, once
// written in Phase 5): Gravity never bundles libx264/libx265 (GPL) itself, so the
// software H.264 path defaults to libopenh264 (BSD-licensed, purpose-built by Cisco for
// exactly this "bundle a permissively-licensed H.264 encoder" scenario) UNLESS the
// resolved ffmpeg binary is one the user pointed at themselves (AdvancedSettings::ffmpegPath)
// and that binary happens to report libx264/libx265 as available -- in which case we use
// it, since Gravity itself still isn't the one shipping the GPL code.
//
// `availableEncoders` empty means "not probed yet" (FFmpegEngine caches this at
// construction; a caller that hasn't probed at all gets the same conservative answer as
// one that probed and found nothing beyond the bundled default).
std::string ResolveVideoEncoder(const std::string& videoCodec, const std::string& hardwareAcceleration,
                                 const std::set<std::string>& availableEncoders) {
    auto has = [&](const std::string& name) { return availableEncoders.count(name) > 0; };

    // "auto"/"h264" is the common case and the one the licensing fallback applies to.
    const bool wantsH264 = videoCodec == "auto" || videoCodec == "h264";
    const bool wantsH265 = videoCodec == "h265";

    if (hardwareAcceleration != "none") {
        const std::string base = wantsH265 ? "hevc" : "h264";
        std::vector<std::string> hwCandidates;
        if (hardwareAcceleration == "auto") {
            hwCandidates = {base + "_nvenc", base + "_amf", base + "_qsv"};
        } else {
            hwCandidates = {base + "_" + hardwareAcceleration};
        }
        for (const auto& candidate : hwCandidates) {
            if (has(candidate)) return candidate;
        }
        // Requested/auto hardware acceleration but nothing usable was found (or never
        // probed) -- fall through to the software path rather than failing the job
        // outright; the caller asked for acceleration as a preference, not a hard
        // requirement (mirrors ProcessingSettings::hardwareAccelerationEnabled being a
        // toggle, not a guarantee -- spec section 22 explicitly forbids assuming
        // hardware encoder availability).
    }

    if (wantsH264) {
        if (has("libx264")) return "libx264";
        return "libopenh264";  // the bundled default; also the answer when unprobed
    }
    if (wantsH265) {
        // No comparably ubiquitous permissively-licensed H.265 encoder exists to bundle
        // as a libopenh264-style default -- request libx265 and let ffmpeg itself report
        // a clear "unknown encoder" error if the resolved binary truly lacks it, rather
        // than silently substituting a different codec than what was asked for.
        return "libx265";
    }
    if (videoCodec == "vp9") return "libvpx-vp9";
    if (videoCodec == "av1") return "libaom-av1";

    return "libopenh264";  // unrecognized videoCodec value -- degrade to the safe default
}

std::string AudioEncoderFor(const std::string& outputFormat) {
    if (outputFormat == "mp3") return "libmp3lame";
    if (outputFormat == "wav") return "pcm_s16le";
    if (outputFormat == "flac") return "flac";
    if (outputFormat == "aac" || outputFormat == "m4a") return "aac";
    if (outputFormat == "ogg") return "libvorbis";
    if (outputFormat == "opus") return "libopus";
    if (outputFormat == "webm") return "libopus";
    return "aac";  // mp4/mov/mkv and anything else not explicitly listed
}

std::string OverlayPositionExpr(const std::string& position) {
    if (position == "top-left") return "10:10";
    if (position == "top-right") return "main_w-overlay_w-10:10";
    if (position == "bottom-left") return "10:main_h-overlay_h-10";
    if (position == "center") return "(main_w-overlay_w)/2:(main_h-overlay_h)/2";
    return "main_w-overlay_w-10:main_h-overlay_h-10";  // bottom-right, and the default
}

bool HasResolution(const MediaProcessingOptions& options) {
    return options.resolutionWidth.has_value() && options.resolutionHeight.has_value();
}

std::string FormatDouble(double value) {
    std::ostringstream out;
    out.precision(3);
    out << std::fixed << value;
    return out.str();
}

}  // namespace

MediaProcessingOptions MediaProcessingOptions::FromJson(const nlohmann::json& json) {
    MediaProcessingOptions options;
    options.outputFormat = json.value("outputFormat", std::string());
    options.quality = json.value("quality", std::string("medium"));
    options.videoCodec = json.value("videoCodec", std::string("auto"));
    options.hardwareAcceleration = json.value("hardwareAcceleration", std::string("auto"));

    if (json.contains("resolution") && json.at("resolution").is_object()) {
        const auto& resolution = json.at("resolution");
        if (resolution.contains("width")) options.resolutionWidth = resolution.at("width").get<int>();
        if (resolution.contains("height")) options.resolutionHeight = resolution.at("height").get<int>();
    }

    if (json.contains("trim") && json.at("trim").is_object()) {
        const auto& trim = json.at("trim");
        if (trim.contains("startSeconds")) options.trimStartSeconds = trim.at("startSeconds").get<double>();
        if (trim.contains("endSeconds")) options.trimEndSeconds = trim.at("endSeconds").get<double>();
    }

    if (json.contains("watermark") && json.at("watermark").is_object()) {
        const auto& watermark = json.at("watermark");
        WatermarkOptions w;
        w.imagePath = watermark.value("imagePath", std::string());
        w.position = watermark.value("position", std::string("bottom-right"));
        w.opacity = watermark.value("opacity", 1.0);
        options.watermark = w;
    }

    if (json.contains("audioBitrateKbps") && !json.at("audioBitrateKbps").is_null()) {
        options.audioBitrateKbps = json.at("audioBitrateKbps").get<int>();
    }
    if (json.contains("videoBitrateKbps") && !json.at("videoBitrateKbps").is_null()) {
        options.videoBitrateKbps = json.at("videoBitrateKbps").get<int>();
    }

    return options;
}

std::vector<std::string> BuildFfmpegArgs(const std::string& inputPath, const std::string& outputPath,
                                          const MediaProcessingOptions& options,
                                          const std::set<std::string>& availableEncoders) {
    if (options.outputFormat.empty()) {
        ThrowInvalidOptions("outputFormat is required");
    }

    std::vector<std::string> args = {"-y", "-hide_banner", "-loglevel", "error"};

    // Trim: -ss/-to placed before -i for fast (keyframe-seek) trimming rather than
    // frame-accurate trimming placed after -i -- a deliberate accuracy/speed tradeoff
    // documented here rather than made configurable, since frame-accurate trim isn't a
    // requirement this product has taken on.
    if (options.trimStartSeconds.has_value()) {
        args.push_back("-ss");
        args.push_back(FormatDouble(*options.trimStartSeconds));
    }
    if (options.trimEndSeconds.has_value()) {
        args.push_back("-to");
        args.push_back(FormatDouble(*options.trimEndSeconds));
    }

    args.push_back("-i");
    args.push_back(inputPath);

    const bool audioOnly = IsAudioOnlyFormat(options.outputFormat);
    const bool isGif = options.outputFormat == "gif";

    if (audioOnly) {
        // No video stream at all: drop it explicitly (-vn) and encode audio only. Trim,
        // resolution and watermark are all video-only options and are silently ignored
        // for an audio-only target -- there is nothing for them to apply to.
        args.push_back("-vn");
        args.push_back("-c:a");
        args.push_back(AudioEncoderFor(options.outputFormat));
        if (options.audioBitrateKbps.has_value() && *options.audioBitrateKbps > 0) {
            args.push_back("-b:a");
            args.push_back(std::to_string(*options.audioBitrateKbps) + "k");
        }
        args.push_back("-progress");
        args.push_back("pipe:1");
        args.push_back(outputPath);
        return args;
    }

    if (isGif) {
        // Naive `-c:v gif` output is low-quality (a fixed 256-color web-safe-ish
        // palette); build a real two-pass-equivalent palette pipeline in one invocation
        // via split+palettegen+paletteuse, optionally preceded by a resolution scale
        // (GIF+watermark together is an unsupported combination -- out of scope, not
        // silently wrong: watermark is simply ignored for a GIF target).
        std::ostringstream filter;
        std::string scaleStage = "scale=iw:ih";
        if (options.resolutionWidth.has_value() && options.resolutionHeight.has_value()) {
            scaleStage = "scale=" + std::to_string(*options.resolutionWidth) + ":" +
                         std::to_string(*options.resolutionHeight);
        }
        filter << "[0:v] fps=15," << scaleStage
               << ":flags=lanczos,split [a][b];[a] palettegen [p];[b][p] paletteuse";
        args.push_back("-filter_complex");
        args.push_back(filter.str());
        args.push_back("-progress");
        args.push_back("pipe:1");
        args.push_back(outputPath);
        return args;
    }

    if (IsImageFormat(options.outputFormat)) {
        // A single still frame: no audio, no CRF/codec-family video-encoder story --
        // just an optional resize/watermark filter graph plus this format's own
        // quality flag (if it has one).
        if (options.watermark.has_value() && !options.watermark->imagePath.empty()) {
            args.push_back("-i");
            args.push_back(options.watermark->imagePath);

            std::ostringstream filter;
            std::string videoLabel = "[0:v]";
            if (HasResolution(options)) {
                filter << "[0:v]scale=" << *options.resolutionWidth << ":" << *options.resolutionHeight
                       << "[scaled];";
                videoLabel = "[scaled]";
            }
            filter << "[1:v]format=rgba,colorchannelmixer=aa=" << FormatDouble(options.watermark->opacity)
                   << "[wm];" << videoLabel << "[wm]overlay=" << OverlayPositionExpr(options.watermark->position)
                   << "[vout]";

            args.push_back("-filter_complex");
            args.push_back(filter.str());
            args.push_back("-map");
            args.push_back("[vout]");
        } else if (HasResolution(options)) {
            args.push_back("-vf");
            args.push_back("scale=" + std::to_string(*options.resolutionWidth) + ":" +
                            std::to_string(*options.resolutionHeight));
        }

        for (const auto& arg : ImageQualityArgs(options.outputFormat, options.quality)) {
            args.push_back(arg);
        }
        args.push_back(outputPath);
        return args;
    }

    // Video (+ optionally audio) target: resolve the encoder, quality, resolution, and
    // watermark into one filter graph + encoder args.
    const bool wideRangeCrf = options.videoCodec == "vp9" || options.videoCodec == "av1";
    const std::string videoEncoder =
        ResolveVideoEncoder(options.videoCodec, options.hardwareAcceleration, availableEncoders);
    const bool isHardwareEncoder = videoEncoder.find("_nvenc") != std::string::npos ||
                                    videoEncoder.find("_amf") != std::string::npos ||
                                    videoEncoder.find("_qsv") != std::string::npos;

    const bool wantsScale = options.resolutionWidth.has_value() && options.resolutionHeight.has_value();

    if (options.watermark.has_value() && !options.watermark->imagePath.empty()) {
        // Watermark needs a second input and a filter_complex graph: optionally scale
        // [0:v] first, overlay the (opacity-adjusted) watermark on top, then map the
        // result explicitly since filter_complex output no longer flows through the
        // implicit default stream selection.
        args.push_back("-i");
        args.push_back(options.watermark->imagePath);

        std::ostringstream filter;
        std::string videoLabel = "[0:v]";
        if (wantsScale) {
            filter << "[0:v]scale=" << *options.resolutionWidth << ":" << *options.resolutionHeight << "[scaled];";
            videoLabel = "[scaled]";
        }
        filter << "[1:v]format=rgba,colorchannelmixer=aa=" << FormatDouble(options.watermark->opacity) << "[wm];"
               << videoLabel << "[wm]overlay=" << OverlayPositionExpr(options.watermark->position) << "[vout]";

        args.push_back("-filter_complex");
        args.push_back(filter.str());
        args.push_back("-map");
        args.push_back("[vout]");
        args.push_back("-map");
        args.push_back("0:a?");  // optional: still succeeds if the input has no audio stream
    } else if (wantsScale) {
        args.push_back("-vf");
        args.push_back("scale=" + std::to_string(*options.resolutionWidth) + ":" +
                        std::to_string(*options.resolutionHeight));
    }

    args.push_back("-c:v");
    args.push_back(videoEncoder);

    // Rate control, in priority order (issue #80):
    //
    //  1. An explicit videoBitrateKbps target always wins. Every encoder understands
    //     -b:v, including the ones that ignore -crf, and it is the only form of rate
    //     control that can make a promise about OUTPUT SIZE rather than output quality.
    //     -maxrate/-bufsize cap the peaks so a high-motion passage can't blow past the
    //     target the caller sized the job around; a 2x bufsize is the conventional
    //     one-second-at-2x-target VBV window.
    //  2. Otherwise -crf, but ONLY for an encoder that actually implements it. Emitting
    //     it for one that doesn't is worse than emitting nothing, because it looks like
    //     a working quality control while silently doing nothing at all.
    //  3. Otherwise nothing: no rate-control flag the resolved encoder would honour.
    //     MediaProcessingJob avoids reaching this branch for anything it can probe, by
    //     supplying (1) instead.
    // > 0, not just has_value(): videoBitrateKbps is read straight out of the caller's
    // options JSON (see FromJson), so a zero or negative value is reachable from the IPC
    // boundary. Emitting one produces "-b:v -5000k", where ffmpeg reads the value as the
    // start of another flag -- a malformed command line built from a number nobody
    // checked. Falling through to CRF is the right degradation: the caller asked for a
    // rate-control target that is not one.
    if (options.videoBitrateKbps.has_value() && *options.videoBitrateKbps > 0) {
        const int kbps = *options.videoBitrateKbps;
        args.push_back("-b:v");
        args.push_back(std::to_string(kbps) + "k");
        args.push_back("-maxrate");
        args.push_back(std::to_string(kbps) + "k");
        args.push_back("-bufsize");
        args.push_back(std::to_string(kbps * 2) + "k");
    } else if (!isHardwareEncoder && EncoderSupportsCrf(videoEncoder)) {
        args.push_back("-crf");
        args.push_back(std::to_string(CrfForQuality(options.quality, wideRangeCrf)));
    }

    args.push_back("-c:a");
    args.push_back(AudioEncoderFor(options.outputFormat));
    // Same bound as videoBitrateKbps above, and for the same reason.
    if (options.audioBitrateKbps.has_value() && *options.audioBitrateKbps > 0) {
        args.push_back("-b:a");
        args.push_back(std::to_string(*options.audioBitrateKbps) + "k");
    }

    // -progress pipe:1: machine-readable periodic progress on stdout, consumed by
    // FFmpegProgressParser exactly the way DownloadJob already consumes yt-dlp's.
    args.push_back("-progress");
    args.push_back("pipe:1");

    args.push_back(outputPath);
    return args;
}

}  // namespace mediatool::media
