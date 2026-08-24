#include "engines/ffmpeg/FFmpegEngine.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <memory>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

#include "core/errors/MediaToolException.h"
#include "core/filesystem/AtomicWriter.h"
#include "core/media/ProcessingOptions.h"
#include "engines/ffmpeg/FFmpegArgumentBuilder.h"
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

void FFmpegEngine::RunEncode(
    const std::string& operation, const std::string& inputPath, const std::string& outputPath,
    const std::function<std::vector<std::string>(const std::string&, const filesystem::FileInfo&)>&
        buildArgs,
    ProgressCallback onProgress, CancelledCallback isCancelled) {
    const auto cancelled = [&isCancelled] { return isCancelled && isCancelled(); };
    const auto report = [&onProgress](const jobs::Progress& progress) {
        if (onProgress) onProgress(progress);
    };
    const auto throwCancelled = [&operation]() {
        throw MediaToolException(ErrorInfo::Make("E_FFMPEG_CANCELLED", ErrorCategory::Cancelled,
                                                  operation + " was cancelled."));
    };

    std::error_code inputEc;
    if (!fs::exists(inputPath, inputEc) || inputEc) {
        throw MediaToolException(ErrorInfo::Make("E_INPUT_NOT_FOUND", ErrorCategory::FileNotFound,
                                                  "The input file no longer exists.",
                                                  "path=" + inputPath));
    }
    // Reading and writing the same file in one ffmpeg pass truncates the input before it
    // has been read. Catch it here rather than letting ffmpeg destroy the source.
    std::error_code sameEc;
    if (fs::exists(outputPath, sameEc) && fs::equivalent(inputPath, outputPath, sameEc) && !sameEc) {
        throw MediaToolException(ErrorInfo::Make(
            "E_SAME_INPUT_OUTPUT", ErrorCategory::UnsupportedFormat,
            "The output file would overwrite the input file.", "path=" + inputPath));
    }

    auto ffmpegPath = ResolveFfmpegPath();
    if (!ffmpegPath.has_value()) {
        throw MediaToolException(ErrorInfo::Make("E_FFMPEG_NOT_FOUND", ErrorCategory::EngineFailure,
                                                  "ffmpeg was not found on this system", "",
                                                  /*recoverable=*/true));
    }

    if (cancelled()) throwCancelled();

    // The duration is what turns ffmpeg's out_time into a percentage. A file we cannot
    // probe is not fatal -- the encode still runs, just without a percentage.
    filesystem::FileInfo probed;
    try {
        probed = Probe(inputPath);
    } catch (const MediaToolException&) {
        probed.path = inputPath;
    }

    const std::string parentDirectory = fs::path(outputPath).parent_path().string();
    if (!parentDirectory.empty()) {
        std::error_code createEc;
        fs::create_directories(parentDirectory, createEc);
        if (createEc) {
            throw MediaToolException(ErrorInfo::Make(
                "E_OUTPUT_DIRECTORY_UNUSABLE", ErrorCategory::PermissionError,
                "The output folder could not be created.",
                "path=" + parentDirectory + " error=" + createEc.message()));
        }
    }

    // Everything ffmpeg writes goes to the temporary path; the destructor removes it if we
    // never reach Commit(), so a cancellation/failure/throw leaves no partial file behind.
    filesystem::AtomicWriter writer(outputPath);
    const std::vector<std::string> args = buildArgs(writer.TemporaryPath(), probed);

    FFmpegProgressParser parser(probed.durationSeconds);
    std::mutex progressMutex;
    std::vector<jobs::Progress> pendingProgress;
    std::vector<std::string> stderrLines;
    std::atomic<bool> sawProgressBlock{false};

    std::unique_ptr<process::IProcess> proc;
    try {
        proc = runner_.Start(
            *ffmpegPath, args, process::ProcessOptions{},
            [&](const std::string& line) {
                std::lock_guard<std::mutex> lock(progressMutex);
                parser.FeedLine(line);
                if (auto progress = parser.TakeProgressIfReady()) {
                    progress->statusMessage = operation;
                    pendingProgress.push_back(*progress);
                    sawProgressBlock = true;
                }
            },
            [&](const std::string& line) {
                std::lock_guard<std::mutex> lock(progressMutex);
                // ffmpeg's stderr at -loglevel error is the diagnostic we surface when the
                // exit code is non-zero. Bound it so a pathological run cannot grow without
                // limit; the first lines are the ones that explain the failure.
                if (stderrLines.size() < 100) stderrLines.push_back(line);
            });
    } catch (const MediaToolException& e) {
        throw MediaToolException(ErrorInfo::Make(
            "E_FFMPEG_LAUNCH_FAILED", ErrorCategory::EngineFailure,
            "ffmpeg could not be started.", e.Info().details.empty() ? e.Info().message
                                                                    : e.Info().details));
    }
    if (!proc) {
        throw MediaToolException(ErrorInfo::Make("E_FFMPEG_LAUNCH_FAILED",
                                                  ErrorCategory::EngineFailure,
                                                  "ffmpeg could not be started.",
                                                  "IProcessRunner::Start returned null"));
    }

    // Bounded escalation: ask nicely, wait a bounded grace period, then kill, then confirm.
    // Never an unbounded wait -- a wedged child must not hold a scheduler slot forever.
    const auto stopProcess = [&] {
        proc->Terminate();
        if (!proc->WaitFor(terminateGraceMs_).has_value()) {
            proc->Kill();
            proc->WaitFor(terminateGraceMs_);
        }
    };

    constexpr int kPollIntervalMs = 100;
    int msSinceProgress = 0;
    std::optional<process::ProcessResult> result;
    bool stalled = false;

    while (true) {
        {
            std::vector<jobs::Progress> drained;
            {
                std::lock_guard<std::mutex> lock(progressMutex);
                drained.swap(pendingProgress);
            }
            if (!drained.empty()) msSinceProgress = 0;
            for (const auto& progress : drained) report(progress);
        }

        if (cancelled()) {
            stopProcess();
            throwCancelled();
        }

        result = proc->WaitFor(kPollIntervalMs);
        if (result.has_value()) break;

        msSinceProgress += kPollIntervalMs;
        if (stallTimeoutMs_ > 0 && msSinceProgress >= stallTimeoutMs_) {
            stalled = true;
            stopProcess();
            result = proc->WaitFor(terminateGraceMs_);
            break;
        }
    }

    // Drain whatever arrived between the last poll and the process exiting.
    {
        std::vector<jobs::Progress> drained;
        {
            std::lock_guard<std::mutex> lock(progressMutex);
            drained.swap(pendingProgress);
        }
        for (const auto& progress : drained) report(progress);
    }

    if (stalled) {
        throw MediaToolException(ErrorInfo::Make(
            "E_FFMPEG_STALLED", ErrorCategory::EngineFailure,
            "ffmpeg stopped responding and was terminated.",
            "no progress for " + std::to_string(stallTimeoutMs_) + "ms; produced " +
                (sawProgressBlock ? "some" : "no") + " progress before stalling"));
    }

    // A cancellation that lands while ffmpeg is exiting still means cancelled, not failed:
    // reporting E_FFMPEG_FAILED for a process the user asked us to stop would be wrong.
    if (cancelled()) throwCancelled();

    const int exitCode = result.has_value() ? result->exitCode : -1;
    if (exitCode != 0) {
        std::string details;
        {
            std::lock_guard<std::mutex> lock(progressMutex);
            details = JoinLines(stderrLines);
        }
        throw MediaToolException(ErrorInfo::Make(
            "E_FFMPEG_FAILED", ErrorCategory::EngineFailure,
            "ffmpeg could not " + ToLower(operation) + " this file.",
            "exit code " + std::to_string(exitCode) + "\n" + details));
    }

    // "ffmpeg exited 0" is not proof of a usable file -- verify before committing.
    std::error_code tempEc;
    if (!fs::exists(writer.TemporaryPath(), tempEc) || tempEc) {
        throw MediaToolException(ErrorInfo::Make(
            "E_OUTPUT_MISSING", ErrorCategory::EngineFailure,
            "ffmpeg reported success but produced no output file.",
            "expected at " + writer.TemporaryPath()));
    }
    std::error_code sizeEc;
    if (fs::file_size(writer.TemporaryPath(), sizeEc) == 0 || sizeEc) {
        throw MediaToolException(ErrorInfo::Make("E_OUTPUT_EMPTY", ErrorCategory::EngineFailure,
                                                  "ffmpeg produced an empty output file.",
                                                  writer.TemporaryPath()));
    }
    try {
        Probe(writer.TemporaryPath());
    } catch (const MediaToolException& e) {
        throw MediaToolException(ErrorInfo::Make(
            "E_OUTPUT_VERIFICATION_FAILED", ErrorCategory::InvalidFile,
            "The produced file failed verification and was discarded.", e.Info().details));
    }

    writer.Commit();

    jobs::Progress done;
    done.percentage = 100.0;
    done.statusMessage = operation + " complete";
    report(done);
}

void FFmpegEngine::Convert(const std::string& inputPath, const std::string& outputPath,
                           const nlohmann::json& options, ProgressCallback onProgress,
                           CancelledCallback isCancelled) {
    const ConversionRequest request = ConversionRequest::FromJson(options);
    RunEncode(
        "Converting", inputPath, outputPath,
        [&inputPath, &request](const std::string& tempPath, const filesystem::FileInfo&) {
            return BuildConversionArgs(inputPath, tempPath, request);
        },
        std::move(onProgress), std::move(isCancelled));
}

void FFmpegEngine::Compress(const std::string& inputPath, const std::string& outputPath,
                            const nlohmann::json& options, ProgressCallback onProgress,
                            CancelledCallback isCancelled) {
    const CompressionRequest request = CompressionRequest::FromJson(options);
    RunEncode(
        "Compressing", inputPath, outputPath,
        [&inputPath, &request](const std::string& tempPath, const filesystem::FileInfo& probed) {
            return BuildCompressionArgs(inputPath, tempPath, request, probed.videoCodec.has_value());
        },
        std::move(onProgress), std::move(isCancelled));
}

void FFmpegEngine::ThrowNotImplemented(const std::string& operation) const {
    throw MediaToolException(ErrorInfo::Make(
        "E_NOT_IMPLEMENTED", ErrorCategory::UnsupportedFormat,
        operation + " is not implemented yet.",
        "FFmpegEngine::" + operation + " is declared for interface completeness and reported "
        "honestly rather than faked; see docs/roadmap.md",
        /*recoverable=*/false));
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
