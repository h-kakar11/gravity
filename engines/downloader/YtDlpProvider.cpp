#include "engines/downloader/YtDlpProvider.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

#include "core/downloads/NdjsonLineProtocol.h"
#include "core/errors/MediaToolException.h"
#include "engines/downloader/YtDlpFormatSelector.h"

namespace mediatool::downloader {

namespace {

bool StartsWithCaseInsensitive(const std::string& value, const std::string& prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    return std::equal(prefix.begin(), prefix.end(), value.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

std::optional<std::uint64_t> OptionalUInt64(const nlohmann::json& data, const char* key) {
    if (!data.contains(key) || data.at(key).is_null()) {
        return std::nullopt;
    }
    return data.at(key).get<std::uint64_t>();
}

std::optional<double> OptionalDouble(const nlohmann::json& data, const char* key) {
    if (!data.contains(key) || data.at(key).is_null()) {
        return std::nullopt;
    }
    return data.at(key).get<double>();
}

std::optional<int> OptionalInt(const nlohmann::json& data, const char* key) {
    if (!data.contains(key) || data.at(key).is_null()) {
        return std::nullopt;
    }
    return data.at(key).get<int>();
}

std::optional<std::string> OptionalString(const nlohmann::json& data, const char* key) {
    if (!data.contains(key) || data.at(key).is_null()) {
        return std::nullopt;
    }
    return data.at(key).get<std::string>();
}

downloads::DownloadFormat ParseDownloadFormat(const nlohmann::json& f) {
    downloads::DownloadFormat format;
    format.formatId = f.value("formatId", std::string());
    format.extension = OptionalString(f, "extension");
    format.resolution = OptionalString(f, "resolution");
    format.width = OptionalInt(f, "width");
    format.height = OptionalInt(f, "height");
    format.fps = OptionalDouble(f, "fps");
    format.videoCodec = OptionalString(f, "videoCodec");
    format.audioCodec = OptionalString(f, "audioCodec");
    format.videoBitrateKbps = OptionalDouble(f, "videoBitrateKbps");
    format.audioBitrateKbps = OptionalDouble(f, "audioBitrateKbps");
    format.filesizeBytes = OptionalUInt64(f, "filesizeBytes");
    format.approxFilesizeBytes = OptionalUInt64(f, "approxFilesizeBytes");
    format.hasVideo = f.value("hasVideo", false);
    format.hasAudio = f.value("hasAudio", false);
    return format;
}

// Used for BOTH the lightweight metadata event Download() emits mid-flight (title/
// duration/playlist fields only) and the rich one Inspect() emits (everything, including
// formats) -- fields absent from `data` simply stay nullopt/empty.
downloads::DownloadMetadata ParseDownloadMetadata(const nlohmann::json& data) {
    downloads::DownloadMetadata metadata;
    metadata.title = data.value("title", std::string());
    metadata.uploader = OptionalString(data, "uploader");
    metadata.durationSeconds = OptionalDouble(data, "duration");
    metadata.webpageUrl = OptionalString(data, "webpageUrl");
    metadata.thumbnailUrl = OptionalString(data, "thumbnailUrl");
    metadata.extractor = OptionalString(data, "extractor");
    metadata.playlistIndex = OptionalInt(data, "playlistIndex");
    metadata.playlistCount = OptionalInt(data, "playlistCount");
    if (data.contains("formats") && data.at("formats").is_array()) {
        for (const auto& f : data.at("formats")) {
            metadata.formats.push_back(ParseDownloadFormat(f));
        }
    }
    return metadata;
}

}  // namespace

YtDlpProvider::YtDlpProvider(process::IProcessRunner& processRunner, std::string pythonExecutable,
                              std::string scriptPath, std::string ffmpegLocation,
                              DownloaderTimeouts timeouts)
    : processRunner_(processRunner),
      pythonExecutable_(std::move(pythonExecutable)),
      scriptPath_(std::move(scriptPath)),
      ffmpegLocation_(std::move(ffmpegLocation)),
      timeouts_(timeouts) {}

bool YtDlpProvider::CanHandle(const std::string& url) const {
    return StartsWithCaseInsensitive(url, "http://") || StartsWithCaseInsensitive(url, "https://");
}

void YtDlpProvider::StopChild(process::IProcess& child) {
    child.Terminate();
    if (!child.WaitFor(2000)) {
        child.Kill();
        (void)child.Wait();
    }
}

YtDlpProvider::RunOutcome YtDlpProvider::RunPythonCommand(
    const nlohmann::json& command,
    const std::function<void(downloads::DownloaderEventType, const nlohmann::json&)>& onEvent,
    downloads::CancelledCallback isCancelled, const char* cancelCode, const char* cancelMessage,
    std::optional<std::chrono::milliseconds> deadline, const char* timeoutCode,
    const char* timeoutMessage) {
    // IProcessRunner may deliver stdout lines from a background thread while this
    // function polls WaitFor() on the caller's thread -- guard the two flags shared
    // between them.
    std::mutex stateMutex;
    bool completedReceived = false;
    std::optional<errors::MediaToolException> pendingError;
    std::deque<std::string> stderrRing;
    static constexpr std::size_t kMaxStderrLines = 20;

    auto handleLine = [&](const std::string& line) {
        const auto parsed = downloads::ParseNdjsonLine(line);
        if (!parsed) {
            return;  // malformed line from the subprocess must not crash the core
        }
        if (!parsed->contains("data")) {
            return;
        }
        const nlohmann::json& data = parsed->at("data");
        const auto type = downloads::GetDownloaderEventType(*parsed);

        if (type == downloads::DownloaderEventType::Completed) {
            std::lock_guard<std::mutex> lock(stateMutex);
            completedReceived = true;
        } else if (type == downloads::DownloaderEventType::Error) {
            std::lock_guard<std::mutex> lock(stateMutex);
            pendingError = errors::MediaToolException(errors::ErrorInfo::FromJson(data));
        }

        if (onEvent) onEvent(type, data);
    };

    auto captureStderr = [&](const std::string& line) {
        // downloader.py's own debug/log chatter; not part of the NDJSON protocol. Kept
        // only as a bounded tail for diagnostics if the process exits without ever
        // emitting a structured error event -- see RunOutcome::stderrTail.
        std::lock_guard<std::mutex> lock(stateMutex);
        stderrRing.push_back(line);
        if (stderrRing.size() > kMaxStderrLines) {
            stderrRing.pop_front();
        }
    };

    process::ProcessOptions processOptions;
    std::unique_ptr<process::IProcess> child = processRunner_.Start(
        pythonExecutable_, {scriptPath_, "--command-stdin"}, processOptions, handleLine, captureStderr);

    child->WriteLine(command.dump());
    child->CloseStdin();

    // steady_clock, not system_clock: a deadline must not be shortened or extended by an
    // NTP correction or a DST change landing mid-fetch.
    const std::chrono::steady_clock::time_point startedAt = std::chrono::steady_clock::now();

    process::ProcessResult result;
    bool finished = false;
    while (!finished) {
        if (isCancelled && isCancelled()) {
            StopChild(*child);
            throw errors::MediaToolException(errors::ErrorInfo::Make(
                cancelCode, errors::ErrorCategory::Cancelled, cancelMessage, "", /*recoverable=*/true));
        }

        if (deadline.has_value() && std::chrono::steady_clock::now() - startedAt >= *deadline) {
            StopChild(*child);
            // Recoverable on purpose: a probe that wedged once is exactly the kind of
            // failure that succeeds on a second attempt, and Phase C's retry policy keys
            // off this flag. NetworkError rather than EngineFailure because the thing
            // that stalled is a network fetch, not the interpreter running it.
            throw errors::MediaToolException(errors::ErrorInfo::Make(
                timeoutCode, errors::ErrorCategory::NetworkError, timeoutMessage,
                "downloader.py exceeded its " +
                    std::to_string(
                        std::chrono::duration_cast<std::chrono::seconds>(*deadline).count()) +
                    "s deadline and was stopped.",
                /*recoverable=*/true));
        }

        if (auto finishedResult = child->WaitFor(200)) {
            result = *finishedResult;
            finished = true;
        }
    }

    RunOutcome outcome;
    outcome.processResult = result;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        outcome.error = pendingError;
        outcome.completedReceived = completedReceived;
        for (const std::string& line : stderrRing) {
            outcome.stderrTail += line + "\n";
        }
    }
    return outcome;
}

downloads::DownloaderInfo YtDlpProvider::Info() {
    downloads::DownloaderInfo info;  // available = false until the probe says otherwise

    nlohmann::json command;
    command["command"] = "version";
    command["params"] = nlohmann::json::object();

    auto onEvent = [&info](downloads::DownloaderEventType type, const nlohmann::json& data) {
        // downloader.py emits this as its own event kind, which GetDownloaderEventType
        // does not know -- the generic branch is where forward-compatible event types
        // land, and reading the payload shape is enough to identify it.
        if (type != downloads::DownloaderEventType::Unknown) return;
        if (!data.contains("available")) return;
        info.available = data.value("available", false);
        info.version = OptionalString(data, "ytDlpVersion");
        info.ageDays = OptionalInt(data, "ageDays");
        info.stale = data.value("stale", false);
    };

    try {
        const RunOutcome outcome =
            RunPythonCommand(command, onEvent, nullptr, "E_VERSION_CANCELLED",
                              "Version probe was cancelled.", timeouts_.version,
                              "E_VERSION_TIMEOUT", "Timed out probing the downloader.");
        if (!outcome.completedReceived) {
            info.available = false;
        }
    } catch (const errors::MediaToolException&) {
        // A python that will not start, a script that is not there, a probe that hung.
        // "Not usable" is the answer, not an error to propagate -- the caller asked
        // whether the downloader works, and it does not.
        info.available = false;
    }
    return info;
}

downloads::DownloadMetadata YtDlpProvider::Inspect(const std::string& url,
                                                    downloads::CancelledCallback isCancelled) {
    nlohmann::json params;
    params["url"] = url;
    nlohmann::json command;
    command["command"] = "inspect";
    command["params"] = params;

    std::optional<downloads::DownloadMetadata> metadata;
    auto onEvent = [&](downloads::DownloaderEventType type, const nlohmann::json& data) {
        if (type == downloads::DownloaderEventType::Metadata) {
            metadata = ParseDownloadMetadata(data);
        }
    };

    const RunOutcome outcome = RunPythonCommand(
        command, onEvent, isCancelled, "E_INSPECT_CANCELLED", "Inspection was cancelled.",
        timeouts_.inspect, "E_INSPECT_TIMEOUT",
        "Timed out fetching information about this link. The site may be unreachable or "
        "very slow right now.");

    if (outcome.error) {
        throw *outcome.error;
    }

    if (!outcome.completedReceived || !metadata) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_INSPECT_NO_RESULT", errors::ErrorCategory::EngineFailure,
            "Downloader process exited without reporting metadata.",
            "downloader.py exited with code " + std::to_string(outcome.processResult.exitCode) +
                " without emitting a metadata event." +
                (outcome.stderrTail.empty() ? "" : "\nstderr:\n" + outcome.stderrTail),
            /*recoverable=*/false));
    }

    return *metadata;
}

void YtDlpProvider::Download(const downloads::DownloadOptions& options,
                              downloads::MetadataCallback onMetadata,
                              downloads::ProgressCallback onProgress,
                              downloads::CompletedCallback onCompleted,
                              downloads::CancelledCallback isCancelled) {
    nlohmann::json params;
    params["url"] = options.url;
    params["outputDir"] = options.outputDirectory;
    // An explicit formatId (the user picked an exact stream from Inspect()'s format list,
    // issue #31) always wins over the quality preset -- yt-dlp's -f selector accepts a raw
    // format id (or "id1+id2" combo) verbatim, same as any other selector string, so no
    // downloader.py change is needed here.
    if (options.formatId.has_value() && !IsSafeFormatSelector(*options.formatId)) {
        // Checked here, before the process starts, as well as at job creation: this is
        // the last point where the value is still ours, and every path into -f goes
        // through it. See IsSafeFormatSelector for what -f actually accepts.
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_INVALID_FORMAT_ID", errors::ErrorCategory::UnsupportedFormat,
            "That stream selection is not a valid format id.",
            "formatId=" + *options.formatId, /*recoverable=*/false));
    }
    params["formatSelector"] =
        options.formatId.has_value() ? *options.formatId : FormatSelectorForQuality(options.quality);
    params["filenameBase"] = options.filenameBase;
    if (!ffmpegLocation_.empty()) {
        params["ffmpegLocation"] = ffmpegLocation_;
    }
    nlohmann::json command;
    command["command"] = "download";
    command["params"] = params;

    auto onEvent = [&](downloads::DownloaderEventType type, const nlohmann::json& data) {
        switch (type) {
            case downloads::DownloaderEventType::Metadata:
                onMetadata(ParseDownloadMetadata(data));
                break;
            case downloads::DownloaderEventType::Progress: {
                jobs::Progress progress;
                progress.processedBytes = OptionalUInt64(data, "downloadedBytes");
                progress.totalBytes = OptionalUInt64(data, "totalBytes");
                progress.speedBytesPerSecond = OptionalDouble(data, "speedBytesPerSecond");
                progress.etaSeconds = OptionalDouble(data, "etaSeconds");
                if (progress.processedBytes && progress.totalBytes && *progress.totalBytes > 0) {
                    progress.percentage = (static_cast<double>(*progress.processedBytes) /
                                           static_cast<double>(*progress.totalBytes)) * 100.0;
                }
                progress.statusMessage = data.value("statusMessage", std::string("Downloading"));
                onProgress(progress);
                break;
            }
            case downloads::DownloaderEventType::Completed:
                onCompleted(data.value("outputPath", std::string()));
                break;
            case downloads::DownloaderEventType::Error:
            case downloads::DownloaderEventType::Unknown:
            default:
                break;  // handled generically by RunPythonCommand, or forward-compatible no-op
        }
    };

    const RunOutcome outcome =
        RunPythonCommand(command, onEvent, isCancelled, "E_DOWNLOAD_CANCELLED", "Download was cancelled.");

    if (outcome.error) {
        throw *outcome.error;
    }

    if (!outcome.completedReceived) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_DOWNLOAD_NO_RESULT", errors::ErrorCategory::EngineFailure,
            "Downloader process exited without reporting completion.",
            "downloader.py exited with code " + std::to_string(outcome.processResult.exitCode) +
                " without emitting a completed or error event." +
                (outcome.stderrTail.empty() ? "" : "\nstderr:\n" + outcome.stderrTail),
            /*recoverable=*/false));
    }
}

}  // namespace mediatool::downloader
