#include "engines/downloader/YtDlpProvider.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

#include "core/downloads/NdjsonLineProtocol.h"
#include "core/errors/MediaToolException.h"

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

}  // namespace

YtDlpProvider::YtDlpProvider(process::IProcessRunner& processRunner, std::string pythonExecutable,
                              std::string scriptPath)
    : processRunner_(processRunner),
      pythonExecutable_(std::move(pythonExecutable)),
      scriptPath_(std::move(scriptPath)) {}

bool YtDlpProvider::CanHandle(const std::string& url) const {
    return StartsWithCaseInsensitive(url, "http://") || StartsWithCaseInsensitive(url, "https://");
}

void YtDlpProvider::Download(const downloads::DownloadOptions& options,
                              downloads::MetadataCallback onMetadata,
                              downloads::ProgressCallback onProgress,
                              downloads::CompletedCallback onCompleted,
                              downloads::CancelledCallback isCancelled) {
    // NOTE: DownloadOptions::extra is not forwarded -- docs/ipc-contract.md's downloader
    // command shape only defines url/outputDir/quality. Revisit if a later phase needs
    // provider-specific overrides to reach downloader.py.
    nlohmann::json params;
    params["url"] = options.url;
    params["outputDir"] = options.outputDirectory;
    params["quality"] = options.quality;
    nlohmann::json command;
    command["command"] = "download";
    command["params"] = params;

    // IProcessRunner may deliver stdout lines from a background thread while this
    // function polls WaitFor() on the caller's thread -- guard the two flags shared
    // between them.
    std::mutex stateMutex;
    bool completedReceived = false;
    std::optional<errors::MediaToolException> pendingError;

    auto handleLine = [&](const std::string& line) {
        const auto parsed = downloads::ParseNdjsonLine(line);
        if (!parsed) {
            return;  // malformed line from the subprocess must not crash the core
        }
        if (!parsed->contains("data")) {
            return;
        }
        const nlohmann::json& data = parsed->at("data");

        switch (downloads::GetDownloaderEventType(*parsed)) {
            case downloads::DownloaderEventType::Metadata: {
                downloads::DownloadMetadata metadata;
                metadata.title = data.value("title", std::string());
                metadata.durationSeconds = OptionalDouble(data, "duration");
                metadata.playlistIndex = OptionalInt(data, "playlistIndex");
                metadata.playlistCount = OptionalInt(data, "playlistCount");
                onMetadata(metadata);
                break;
            }
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
            case downloads::DownloaderEventType::Completed: {
                {
                    std::lock_guard<std::mutex> lock(stateMutex);
                    completedReceived = true;
                }
                onCompleted(data.value("outputPath", std::string()));
                break;
            }
            case downloads::DownloaderEventType::Error: {
                std::lock_guard<std::mutex> lock(stateMutex);
                pendingError = errors::MediaToolException(errors::ErrorInfo::FromJson(data));
                break;
            }
            case downloads::DownloaderEventType::Unknown:
            default:
                break;  // forward-compatible: ignore event names this build doesn't know
        }
    };

    auto ignoreStderr = [](const std::string& /*line*/) {
        // downloader.py's own debug/log chatter; not part of the NDJSON protocol.
    };

    process::ProcessOptions processOptions;
    std::unique_ptr<process::IProcess> child = processRunner_.Start(
        pythonExecutable_, {scriptPath_, "--command-stdin"}, processOptions, handleLine, ignoreStderr);

    child->WriteLine(command.dump());
    child->CloseStdin();

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
            throw errors::MediaToolException(errors::ErrorInfo::Make(
                "E_DOWNLOAD_CANCELLED", errors::ErrorCategory::Cancelled,
                "Download was cancelled.", "", /*recoverable=*/true));
        }

        if (auto finishedResult = child->WaitFor(200)) {
            result = *finishedResult;
            finished = true;
        }
    }

    std::optional<errors::MediaToolException> errorToThrow;
    bool sawCompleted = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        errorToThrow = pendingError;
        sawCompleted = completedReceived;
    }

    if (errorToThrow) {
        throw *errorToThrow;
    }

    if (!sawCompleted) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_DOWNLOAD_NO_RESULT", errors::ErrorCategory::EngineFailure,
            "Downloader process exited without reporting completion.",
            "downloader.py exited with code " + std::to_string(result.exitCode) +
                " without emitting a completed or error event.",
            /*recoverable=*/false));
    }
}

}  // namespace mediatool::downloader
