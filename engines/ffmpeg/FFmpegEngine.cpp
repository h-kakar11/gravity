#include "engines/ffmpeg/FFmpegEngine.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

#include "core/errors/MediaToolException.h"
#include "engines/ffmpeg/FFmpegDiscovery.h"

namespace mediatool::media {

namespace {

namespace fs = std::filesystem;
using errors::ErrorCategory;
using errors::ErrorInfo;
using errors::MediaToolException;

struct ProcessCapture {
    int exitCode = -1;
    std::vector<std::string> stdoutLines;
    std::vector<std::string> stderrLines;
};

ProcessCapture RunAndCapture(process::IProcessRunner& runner, const std::string& executable,
                              const std::vector<std::string>& args) {
    ProcessCapture capture;
    process::ProcessOptions options;
    auto proc = runner.Start(
        executable, args, options,
        [&capture](const std::string& line) { capture.stdoutLines.push_back(line); },
        [&capture](const std::string& line) { capture.stderrLines.push_back(line); });
    if (!proc) {
        throw MediaToolException(ErrorInfo::Make(
            "E_FFMPEG_LAUNCH_FAILED", ErrorCategory::EngineFailure,
            "Failed to launch " + executable, "IProcessRunner::Start returned null"));
    }
    auto result = proc->Wait();
    capture.exitCode = result.exitCode;
    return capture;
}

std::string JoinLines(const std::vector<std::string>& lines) {
    std::string joined;
    for (const auto& line : lines) {
        joined += line;
        joined += '\n';
    }
    return joined;
}

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// "30000/1001" -> 29.97..., "0/0" (ffprobe's "unknown" sentinel) -> nullopt.
std::optional<double> ParseFrameRateFraction(const std::string& fraction) {
    auto slash = fraction.find('/');
    if (slash == std::string::npos) return std::nullopt;
    try {
        double numerator = std::stod(fraction.substr(0, slash));
        double denominator = std::stod(fraction.substr(slash + 1));
        if (denominator == 0.0) return std::nullopt;
        return numerator / denominator;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

FFmpegEngine::FFmpegEngine(process::IProcessRunner& runner,
                           std::optional<std::string> overrideFfmpegPath,
                           std::optional<std::string> overrideFfprobePath)
    : runner_(runner),
      overrideFfmpegPath_(std::move(overrideFfmpegPath)),
      overrideFfprobePath_(std::move(overrideFfprobePath)) {}

std::optional<std::string> FFmpegEngine::ResolveFfmpegPath() const {
    return DiscoverFfmpegPath(runner_, overrideFfmpegPath_);
}

std::optional<std::string> FFmpegEngine::ResolveFfprobePath() const {
    return DiscoverFfprobePath(runner_, overrideFfprobePath_);
}

bool FFmpegEngine::IsAvailable() const {
    return ResolveFfmpegPath().has_value() && ResolveFfprobePath().has_value();
}

std::optional<std::string> FFmpegEngine::Version() const {
    auto ffmpegPath = ResolveFfmpegPath();
    if (!ffmpegPath.has_value()) return std::nullopt;
    try {
        auto capture = RunAndCapture(runner_, *ffmpegPath, {"-version"});
        if (capture.exitCode != 0 || capture.stdoutLines.empty()) return std::nullopt;
        return capture.stdoutLines.front();
    } catch (...) {
        return std::nullopt;
    }
}

filesystem::FileInfo FFmpegEngine::Probe(const std::string& path) {
    std::error_code existsError;
    if (!fs::exists(path, existsError) || existsError) {
        throw MediaToolException(ErrorInfo::Make(
            "E_FILE_NOT_FOUND", ErrorCategory::FileNotFound, "File not found: " + path,
            existsError.message()));
    }

    auto ffprobePath = ResolveFfprobePath();
    if (!ffprobePath.has_value()) {
        throw MediaToolException(ErrorInfo::Make(
            "E_FFPROBE_NOT_FOUND", ErrorCategory::EngineFailure,
            "ffprobe was not found on this system", "", /*recoverable=*/true));
    }

    ProcessCapture capture;
    try {
        capture = RunAndCapture(runner_, *ffprobePath,
                                {"-v", "quiet", "-print_format", "json", "-show_format",
                                 "-show_streams", path});
    } catch (const MediaToolException&) {
        throw;
    } catch (const std::exception& ex) {
        throw MediaToolException(ErrorInfo::Make("E_FFPROBE_LAUNCH_FAILED",
                                                  ErrorCategory::EngineFailure,
                                                  "Failed to launch ffprobe", ex.what()));
    }

    if (capture.exitCode != 0) {
        throw MediaToolException(ErrorInfo::Make(
            "E_FFPROBE_FAILED", ErrorCategory::InvalidFile,
            "ffprobe could not analyze this file", JoinLines(capture.stderrLines)));
    }

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(JoinLines(capture.stdoutLines));
    } catch (const std::exception& ex) {
        throw MediaToolException(ErrorInfo::Make(
            "E_FFPROBE_BAD_OUTPUT", ErrorCategory::EngineFailure,
            "ffprobe returned output that could not be parsed as JSON", ex.what()));
    }

    filesystem::FileInfo info;
    info.path = path;

    fs::path fsPath(path);
    info.filename = fsPath.filename().string();
    std::string extension = fsPath.extension().string();
    if (!extension.empty() && extension.front() == '.') extension.erase(0, 1);
    info.extension = ToLower(extension);

    std::error_code sizeError;
    auto size = fs::file_size(path, sizeError);
    info.sizeBytes = sizeError ? 0 : static_cast<std::uint64_t>(size);

    // ffprobe's own JSON shape is trusted, but guard the extraction anyway so a
    // surprising/malformed document becomes a MediaToolException rather than a bare
    // exception escaping past this function (the one-exception-type convention).
    try {
        const nlohmann::json* videoStream = nullptr;
        const nlohmann::json* audioStream = nullptr;
        if (parsed.contains("streams") && parsed["streams"].is_array()) {
            for (const auto& stream : parsed["streams"]) {
                if (!stream.is_object()) continue;
                std::string codecType = stream.value("codec_type", std::string());
                if (codecType == "video" && videoStream == nullptr) {
                    videoStream = &stream;
                } else if (codecType == "audio" && audioStream == nullptr) {
                    audioStream = &stream;
                }
            }
        }

        if (videoStream != nullptr) {
            info.category = filesystem::FileCategory::Video;
        } else if (audioStream != nullptr) {
            info.category = filesystem::FileCategory::Audio;
        } else {
            info.category = filesystem::FileCategory::Unknown;
        }

        if (parsed.contains("format") && parsed["format"].is_object()) {
            const auto& format = parsed["format"];
            if (format.contains("duration") && format["duration"].is_string()) {
                try {
                    info.durationSeconds = std::stod(format["duration"].get<std::string>());
                } catch (...) {
                }
            }
            if (format.contains("bit_rate") && format["bit_rate"].is_string()) {
                try {
                    info.bitrate = std::stoll(format["bit_rate"].get<std::string>());
                } catch (...) {
                }
            }
        }

        if (videoStream != nullptr) {
            if (videoStream->contains("width")) info.width = videoStream->value("width", 0);
            if (videoStream->contains("height")) info.height = videoStream->value("height", 0);
            if (videoStream->contains("codec_name")) {
                info.videoCodec = videoStream->value("codec_name", std::string());
            }
            if (videoStream->contains("r_frame_rate")) {
                info.fps = ParseFrameRateFraction(videoStream->value("r_frame_rate", std::string("0/0")));
            }
        }
        if (audioStream != nullptr && audioStream->contains("codec_name")) {
            info.audioCodec = audioStream->value("codec_name", std::string());
        }
    } catch (const std::exception& ex) {
        throw MediaToolException(ErrorInfo::Make(
            "E_FFPROBE_UNEXPECTED_SHAPE", ErrorCategory::EngineFailure,
            "ffprobe returned a JSON shape that could not be interpreted", ex.what()));
    }

    return info;
}

void FFmpegEngine::ThrowNotImplemented(const std::string& operation) const {
    throw MediaToolException(ErrorInfo::Make(
        "E_NOT_IMPLEMENTED", ErrorCategory::UnsupportedFormat,
        operation + " is not implemented in Phase 1",
        "FFmpegEngine::" + operation + " is intentionally out of scope for Phase 1 (spec section 16)",
        /*recoverable=*/false));
}

void FFmpegEngine::Convert(const std::string&, const std::string&, const nlohmann::json&,
                           ProgressCallback, CancelledCallback) {
    ThrowNotImplemented("Convert");
}

void FFmpegEngine::Compress(const std::string&, const std::string&, const nlohmann::json&,
                            ProgressCallback, CancelledCallback) {
    ThrowNotImplemented("Compress");
}

void FFmpegEngine::ExtractAudio(const std::string&, const std::string&, ProgressCallback,
                                CancelledCallback) {
    ThrowNotImplemented("ExtractAudio");
}

void FFmpegEngine::ExtractFrames(const std::string&, const std::string&, const nlohmann::json&,
                                 ProgressCallback, CancelledCallback) {
    ThrowNotImplemented("ExtractFrames");
}

}  // namespace mediatool::media
