#pragma once

// One of the five mockable interfaces called out in spec section 37. YtDlpProvider
// (engines/downloader) is the Phase 1 implementation, launching python/downloader over
// an IProcessRunner and translating its NDJSON events (docs/ipc-contract.md) into these
// callbacks. Do not make yt-dlp synonymous with the download architecture (spec section
// 19) -- everything outside engines/downloader talks to IDownloadProvider only.

#include <functional>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "core/jobs/Progress.h"

namespace mediatool::downloads {

struct DownloadOptions {
    std::string url;
    std::string outputDirectory;
    std::string quality = "best";
    nlohmann::json extra;  // provider-specific escape hatch, e.g. format selector overrides
};

struct DownloadMetadata {
    std::string title;
    std::optional<double> durationSeconds;
    std::optional<int> playlistIndex;
    std::optional<int> playlistCount;
};

using MetadataCallback = std::function<void(const DownloadMetadata&)>;
using ProgressCallback = std::function<void(const jobs::Progress&)>;
using CompletedCallback = std::function<void(const std::string& outputPath)>;
using CancelledCallback = std::function<bool()>;

class IDownloadProvider {
public:
    virtual ~IDownloadProvider() = default;

    virtual bool CanHandle(const std::string& url) const = 0;

    // Blocking; intended to run on a job's worker thread. Throws
    // errors::MediaToolException on failure. Must poll isCancelled() periodically and
    // stop (throwing with ErrorCategory::Cancelled) when it returns true.
    virtual void Download(const DownloadOptions& options, MetadataCallback onMetadata,
                          ProgressCallback onProgress, CompletedCallback onCompleted,
                          CancelledCallback isCancelled) = 0;
};

}  // namespace mediatool::downloads
