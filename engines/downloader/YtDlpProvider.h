#pragma once

// Phase 1/2 implementation of downloads::IDownloadProvider (core/downloads/IDownloadProvider.h)
// that launches python/downloader/downloader.py over an IProcessRunner and translates its
// NDJSON stdout protocol (docs/protocols/downloader.md) into the interface's callbacks/return
// values. Everything outside this file talks to IDownloadProvider only -- do not treat
// yt-dlp as synonymous with the download architecture (spec section 19).

#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "core/downloads/IDownloadProvider.h"
#include "core/downloads/NdjsonLineProtocol.h"
#include "core/errors/MediaToolException.h"
#include "core/process/IProcessRunner.h"

namespace mediatool::downloader {

class YtDlpProvider : public downloads::IDownloadProvider {
public:
    // `pythonExecutable` and `scriptPath` are injected rather than hardcoded so tests and
    // deployment can point at whatever venv/interpreter and script location apply.
    // `ffmpegLocation` (optional, resolved once at startup via engines/ffmpeg/FFmpegDiscovery
    // -- see app/core/main.cpp) is forwarded to yt-dlp so it merges separate video/audio
    // streams using the SAME ffmpeg binary the rest of the app already resolved, rather
    // than letting yt-dlp run its own independent discovery (docs/decisions.md "Video/audio
    // merge strategy"). Empty means "let yt-dlp fall back to its own PATH search."
    YtDlpProvider(process::IProcessRunner& processRunner, std::string pythonExecutable,
                  std::string scriptPath, std::string ffmpegLocation = "");

    // True for anything that looks like an http/https URL -- deliberately not
    // youtube.com-only, so this extends to other yt-dlp-supported sites later (spec
    // section 20). yt-dlp itself rejects what it can't actually handle.
    bool CanHandle(const std::string& url) const override;

    downloads::DownloadMetadata Inspect(const std::string& url,
                                         downloads::CancelledCallback isCancelled) override;

    void Download(const downloads::DownloadOptions& options, downloads::MetadataCallback onMetadata,
                  downloads::ProgressCallback onProgress, downloads::CompletedCallback onCompleted,
                  downloads::CancelledCallback isCancelled) override;

private:
    // Shared "spawn downloader.py, feed it one command line, drain its NDJSON events
    // until it reports completed/error/exits, honoring cancellation" plumbing used by
    // both Inspect() and Download() -- see docs/protocols/downloader.md. `onEvent` is
    // invoked for every event line (Completed/Error included) so a caller can also
    // extract event-specific data (e.g. Download()'s onCompleted outputPath); this
    // struct's own `completedReceived`/`error` cover the generic "did the process
    // succeed" question so callers don't have to re-derive it.
    struct RunOutcome {
        process::ProcessResult processResult;
        std::optional<errors::MediaToolException> error;
        bool completedReceived = false;
    };

    RunOutcome RunPythonCommand(
        const nlohmann::json& command,
        const std::function<void(downloads::DownloaderEventType, const nlohmann::json& data)>& onEvent,
        downloads::CancelledCallback isCancelled, const char* cancelCode, const char* cancelMessage);

    process::IProcessRunner& processRunner_;
    std::string pythonExecutable_;
    std::string scriptPath_;
    std::string ffmpegLocation_;
};

}  // namespace mediatool::downloader
