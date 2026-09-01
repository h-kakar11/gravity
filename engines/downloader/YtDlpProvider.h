#pragma once

// Phase 1/2 implementation of downloads::IDownloadProvider (core/downloads/IDownloadProvider.h)
// that launches python/downloader/downloader.py over an IProcessRunner and translates its
// NDJSON stdout protocol (docs/protocols/downloader.md) into the interface's callbacks/return
// values. Everything outside this file talks to IDownloadProvider only -- do not treat
// yt-dlp as synonymous with the download architecture (spec section 19).

#include <chrono>
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

// Wall-clock bounds on a whole downloader.py run, as opposed to the per-socket
// `socket_timeout` downloader.py already sets.
//
// Those are not the same guarantee and only one of them is a bound on the caller.
// `socket_timeout` limits ONE connect/read; a metadata fetch is many of them, yt-dlp
// retries each one on its own schedule, and an extractor that keeps returning data
// slowly never trips it at all. Inspect() runs synchronously on a job worker (and, for
// inspectDownloadUrl, with the user staring at a spinner), so "eventually, probably"
// is not good enough: without a deadline here a wedged child holds that thread until
// the process exits, and nothing in the app can tell the user why.
//
// A deadline is deliberately NOT applied to Download(): a legitimate 4K download runs
// for as long as it runs, and killing it on a clock would be a bug, not a safeguard.
struct DownloaderTimeouts {
    // Generous relative to a healthy probe (typically a second or two) because it has
    // to survive yt-dlp's own internal retries without cutting off a slow-but-working
    // extractor -- it exists to bound a hang, not to enforce a latency target.
    std::chrono::milliseconds inspect{std::chrono::seconds(60)};
};

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
                  std::string scriptPath, std::string ffmpegLocation = "",
                  DownloaderTimeouts timeouts = DownloaderTimeouts{});

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
        // Last few lines of downloader.py's stderr (its own debug/log chatter, not part
        // of the NDJSON protocol) -- empty unless the process exits without ever emitting
        // a structured error event, the one case where this is actually attached to an
        // ErrorInfo (issue #24). Previously discarded entirely, which meant that specific
        // failure mode (an unhandled crash outside downloader.py's own try/except) had no
        // diagnostic at all beyond a bare exit code.
        std::string stderrTail;
    };

    // `deadline` (nullopt = none) bounds the whole run; on expiry the child is stopped
    // the same way a cancellation stops it and a MediaToolException carrying
    // `timeoutCode`/`timeoutMessage` is thrown. Cancellation is still checked first --
    // a user who cancelled during the last poll interval gets E_*_CANCELLED, not a
    // timeout they never saw.
    RunOutcome RunPythonCommand(
        const nlohmann::json& command,
        const std::function<void(downloads::DownloaderEventType, const nlohmann::json& data)>& onEvent,
        downloads::CancelledCallback isCancelled, const char* cancelCode, const char* cancelMessage,
        std::optional<std::chrono::milliseconds> deadline = std::nullopt,
        const char* timeoutCode = "", const char* timeoutMessage = "");

    // Stops `child` the way both cancellation and a timeout need it stopped: ask
    // politely, wait briefly, kill if it is still there.
    static void StopChild(process::IProcess& child);

    process::IProcessRunner& processRunner_;
    std::string pythonExecutable_;
    std::string scriptPath_;
    std::string ffmpegLocation_;
    DownloaderTimeouts timeouts_;
};

}  // namespace mediatool::downloader
