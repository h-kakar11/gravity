#include "engines/ffmpeg/FFmpegEngine.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include "core/errors/MediaToolException.h"
#include "core/media/DeferredOperations.h"
#include "engines/ffmpeg/FFmpegErrorClassifier.h"
#include "engines/ffmpeg/FFmpegArgBuilder.h"
#include "engines/ffmpeg/FFmpegDiscovery.h"
#include "engines/ffmpeg/FFmpegProgressParser.h"

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
    std::call_once(ffmpegPathOnce_, [this] {
        ffmpegPathCache_ = DiscoverFfmpegPath(runner_, overrideFfmpegPath_);
    });
    return *ffmpegPathCache_;
}

std::optional<std::string> FFmpegEngine::ResolveFfprobePath() const {
    std::call_once(ffprobePathOnce_, [this] {
        ffprobePathCache_ = DiscoverFfprobePath(runner_, overrideFfprobePath_);
    });
    return *ffprobePathCache_;
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
        // Same table as a failed conversion, different default: ffprobe being unable to
        // read a file IS "this is not a media file", so an unrecognized probe failure is
        // E_INVALID_FILE rather than a generic engine failure. It also used to report the
        // same code whether the file was corrupt, missing, or unreadable by permission.
        throw MediaToolException(
            ClassifyFfprobeFailure(JoinLines(capture.stderrLines), capture.exitCode));
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
    // Code, category and wording all come from the one deferral table
    // (core/media/DeferredOperations.h) so this error is byte-for-byte the one
    // filesystem::DeferredCapabilitiesFor() already told the frontend to expect.
    throw MediaToolException(MakeNotImplementedError(operation));
}

const std::set<std::string>& FFmpegEngine::AvailableEncoders() const {
    // Computed once and cached, not per job -- mirrors the "one discovery path, one
    // lifetime" principle FFmpegDiscovery::DiscoverFfmpegPath already documents. The
    // returned reference stays valid because the cache is assigned exactly once, ever:
    // handing out a reference to something a second thread might be mid-assignment on is
    // precisely what the once_flag prevents.
    std::call_once(encodersOnce_, [this] {
        auto ffmpegPath = ResolveFfmpegPath();
        availableEncodersCache_ =
            ffmpegPath.has_value() ? DiscoverAvailableEncoders(runner_, *ffmpegPath) : std::set<std::string>{};
    });
    return *availableEncodersCache_;
}

void FFmpegEngine::RunFfmpegJob(const std::string& inputPath, const std::string& outputPath,
                                const nlohmann::json& options, ProgressCallback onProgress,
                                CancelledCallback isCancelled) {
    auto ffmpegPath = ResolveFfmpegPath();
    if (!ffmpegPath.has_value()) {
        throw MediaToolException(ErrorInfo::Make(
            "E_FFMPEG_NOT_FOUND", ErrorCategory::EngineFailure,
            "ffmpeg was not found on this system", "", /*recoverable=*/true));
    }

    const MediaProcessingOptions parsedOptions = MediaProcessingOptions::FromJson(options);

    // Probing the input gives FFmpegProgressParser a total duration (for percentage/ETA)
    // and bitrate (for a speedBytesPerSecond estimate) -- best-effort: a probe failure
    // just means progress reports without those fields, not a fatal error for the job.
    std::optional<double> totalDurationSeconds;
    std::optional<double> inputBitrateBps;
    try {
        const filesystem::FileInfo inputInfo = Probe(inputPath);
        totalDurationSeconds = inputInfo.durationSeconds;
        if (inputInfo.bitrate.has_value()) inputBitrateBps = static_cast<double>(*inputInfo.bitrate);
    } catch (const MediaToolException&) {
    }
    if (totalDurationSeconds.has_value()) {
        const double start = parsedOptions.trimStartSeconds.value_or(0.0);
        const double end = parsedOptions.trimEndSeconds.value_or(*totalDurationSeconds);
        totalDurationSeconds = std::max(0.0, end - start);  // trim shortens what ffmpeg actually processes
    }

    const std::vector<std::string> args =
        BuildFfmpegArgs(inputPath, outputPath, parsedOptions, AvailableEncoders());

    FFmpegProgressParser parser(totalDurationSeconds, inputBitrateBps);
    std::mutex progressMutex;  // IProcessRunner may deliver stdout from a background thread

    auto handleStdout = [&](const std::string& line) {
        std::lock_guard<std::mutex> lock(progressMutex);
        parser.FeedLine(line);
        if (auto progress = parser.TakeProgressIfReady(); progress.has_value() && onProgress) {
            onProgress(*progress);
        }
    };
    // ffmpeg's stderr used to be discarded outright, which is why every failure came back
    // as one code and an exit status. It is the only place ffmpeg says WHY it failed --
    // "No space left on device", "Permission denied", "moov atom not found", "Unknown
    // encoder" -- so a bounded tail is kept and classified (FFmpegErrorClassifier.h).
    // Bounded because ffmpeg on a bad input can produce a line per frame, and the last few
    // lines are the ones that carry the failure.
    // Captured by reference, and that is safe by declaration order rather than by luck:
    // RealProcessRunner delivers these callbacks from its drain thread, which is joined by
    // ~IProcess. `child` below is declared AFTER this ring, so it is destroyed BEFORE it --
    // the thread is already joined by the time the ring dies, on the throw path included.
    std::mutex stderrMutex;
    std::deque<std::string> stderrRing;
    static constexpr std::size_t kMaxStderrLines = 20;
    auto captureStderr = [&](const std::string& line) {
        std::lock_guard<std::mutex> lock(stderrMutex);
        stderrRing.push_back(line);
        if (stderrRing.size() > kMaxStderrLines) stderrRing.pop_front();
    };

    process::ProcessOptions processOptions;
    std::unique_ptr<process::IProcess> child =
        runner_.Start(*ffmpegPath, args, processOptions, handleStdout, captureStderr);
    if (!child) {
        throw MediaToolException(ErrorInfo::Make(
            "E_FFMPEG_LAUNCH_FAILED", ErrorCategory::EngineFailure,
            "Failed to launch ffmpeg", "IProcessRunner::Start returned null"));
    }

    // Cooperative cancellation, mirroring YtDlpProvider::RunPythonCommand's convention:
    // Terminate() first, give it a couple seconds, Kill() if it's still not gone.
    process::ProcessResult result;
    bool finished = false;
    while (!finished) {
        if (isCancelled && isCancelled()) {
            child->Terminate();
            if (auto terminated = child->WaitFor(2000)) {
                result = *terminated;
            } else {
                child->Kill();
                result = child->Wait();
            }
            throw MediaToolException(ErrorInfo::Make(
                "E_MEDIA_PROCESSING_CANCELLED", ErrorCategory::Cancelled,
                "Conversion was cancelled.", "", /*recoverable=*/true));
        }
        if (auto finishedResult = child->WaitFor(200)) {
            result = *finishedResult;
            finished = true;
        }
    }

    if (result.exitCode != 0) {
        std::string stderrTail;
        {
            std::lock_guard<std::mutex> lock(stderrMutex);
            for (const std::string& line : stderrRing) stderrTail += line + "\n";
        }
        // Free space at the OUTPUT location, and only to make a disk-full message
        // concrete. Measured after the fact, so it is never what decides the
        // classification -- see ClassifyFfmpegFailure.
        //
        // The empty-parent guard is load-bearing, not defensive habit: outputPath with no
        // directory component ("out.mp4") gives an empty parent_path(), and libstdc++'s
        // WINDOWS fs::space() begins with an internal absolute(p) that throws
        // filesystem_error for an empty path -- ignoring the error_code overload's promise
        // not to throw. POSIX fs::space() reports it through the error_code instead, so
        // this is invisible on Linux and fatal on the only platform Gravity ships to.
        // Same `if (!parentDir.empty())` guard the stores use (JsonFileSettingsStore,
        // InProgressJobStore, JobHistoryStore, PresetStore).
        std::optional<std::uint64_t> availableBytes;
        const fs::path outputDirectory = fs::path(outputPath).parent_path();
        if (!outputDirectory.empty()) {
            std::error_code spaceError;
            const auto space = fs::space(outputDirectory, spaceError);
            if (!spaceError) availableBytes = static_cast<std::uint64_t>(space.available);
        }

        throw MediaToolException(
            ClassifyFfmpegFailure(stderrTail, result.exitCode, availableBytes));
    }
}

void FFmpegEngine::Convert(const std::string& inputPath, const std::string& outputPath,
                           const nlohmann::json& options, ProgressCallback onProgress,
                           CancelledCallback isCancelled) {
    RunFfmpegJob(inputPath, outputPath, options, std::move(onProgress), std::move(isCancelled));
}

void FFmpegEngine::Compress(const std::string& inputPath, const std::string& outputPath,
                            const nlohmann::json& options, ProgressCallback onProgress,
                            CancelledCallback isCancelled) {
    // Compress is Convert with different default option VALUES (supplied by
    // MediaProcessingJob), not a structurally different ffmpeg invocation -- see
    // FFmpegArgBuilder.h.
    RunFfmpegJob(inputPath, outputPath, options, std::move(onProgress), std::move(isCancelled));
}

void FFmpegEngine::ExtractAudio(const std::string&, const std::string&, ProgressCallback,
                                CancelledCallback) {
    ThrowNotImplemented(kExtractAudioOperation);
}

void FFmpegEngine::ExtractFrames(const std::string&, const std::string&, const nlohmann::json&,
                                 ProgressCallback, CancelledCallback) {
    ThrowNotImplemented(kExtractFramesOperation);
}

}  // namespace mediatool::media
