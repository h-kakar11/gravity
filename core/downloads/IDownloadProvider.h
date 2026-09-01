#pragma once

// One of the five mockable interfaces called out in spec section 37. YtDlpProvider
// (engines/downloader) is the Phase 1/2 implementation, launching python/downloader over
// an IProcessRunner and translating its NDJSON events (docs/ipc-contract.md,
// docs/protocols/downloader.md) into these callbacks/return values. Do not make yt-dlp
// synonymous with the download architecture (spec section 19) -- everything outside
// engines/downloader talks to IDownloadProvider only.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/downloads/QualityPreset.h"
#include "core/jobs/Progress.h"

namespace mediatool::downloads {

struct DownloadOptions {
    std::string url;
    std::string outputDirectory;
    QualityPreset quality = QualityPreset::Best;
    // Already sanitized and collision-checked by the caller (see
    // filesystem::DeduplicateBaseName) -- must NOT include an extension, since the final
    // container is chosen by yt-dlp itself once the merge (if any) is complete.
    std::string filenameBase;
    // Explicit yt-dlp format id (or id combo, e.g. "137+140") from Inspect()'s own
    // DownloadFormat list (issue #31). When set, this overrides `quality` entirely -- the
    // caller picked an exact stream, so there's no preset to derive a selector from.
    std::optional<std::string> formatId;
    nlohmann::json extra;  // provider-specific escape hatch
};

// One selectable stream as reported by the provider during Inspect() (spec section 7).
// Providers may leave any field but formatId/hasVideo/hasAudio unset -- real-world
// extractors frequently omit bitrate, exact resolution, or filesize.
struct DownloadFormat {
    std::string formatId;
    std::optional<std::string> extension;
    std::optional<std::string> resolution;  // e.g. "1920x1080", video formats only
    std::optional<int> width;
    std::optional<int> height;
    std::optional<double> fps;
    std::optional<std::string> videoCodec;
    std::optional<std::string> audioCodec;
    std::optional<double> videoBitrateKbps;
    std::optional<double> audioBitrateKbps;
    std::optional<std::uint64_t> filesizeBytes;
    std::optional<std::uint64_t> approxFilesizeBytes;
    bool hasVideo = false;
    bool hasAudio = false;
};

struct DownloadMetadata {
    std::string title;
    std::optional<std::string> uploader;
    std::optional<double> durationSeconds;
    std::optional<std::string> webpageUrl;
    std::optional<std::string> thumbnailUrl;
    std::optional<std::string> extractor;
    std::optional<int> playlistIndex;
    std::optional<int> playlistCount;
    // Populated by Inspect(); left empty by the lightweight metadata event Download()
    // emits mid-flight (it already had the format list before starting).
    std::vector<DownloadFormat> formats;
};

// What the downloader backend actually is on this machine, and whether it is healthy
// enough to be worth using. Reported before a download fails for a reason the user cannot
// see: yt-dlp's extractors for the big sites break on a scale of weeks, so a build a couple
// of years old fails on real URLs with messages ("video unavailable") that blame the video
// rather than the tool.
struct DownloaderInfo {
    // False means the backend cannot run at all -- the interpreter is missing, or its
    // library is not installed. Everything below is then unset.
    bool available = false;
    std::string backend = "yt-dlp";
    std::optional<std::string> version;
    // Days since the backend's own release date, when its version string carries one.
    std::optional<int> ageDays;
    // True when `ageDays` is past the point where extractor breakage is near-certain.
    bool stale = false;

    nlohmann::json ToJson() const;
};

using MetadataCallback = std::function<void(const DownloadMetadata&)>;
using ProgressCallback = std::function<void(const jobs::Progress&)>;
using CompletedCallback = std::function<void(const std::string& outputPath)>;
using CancelledCallback = std::function<bool()>;

class IDownloadProvider {
public:
    virtual ~IDownloadProvider() = default;

    virtual bool CanHandle(const std::string& url) const = 0;

    // Cheap enough to call on demand but not free (it starts a process), so callers are
    // expected to cache it. Never throws: "the downloader is not usable" is the answer
    // this question exists to give, not an error condition.
    virtual DownloaderInfo Info() = 0;

    // Blocking; fetches full metadata -- including the available formats a caller needs
    // for quality selection -- WITHOUT downloading anything (spec section 5/6). Throws
    // errors::MediaToolException on failure (unsupported/playlist URL, private/removed/
    // geo-restricted video, network failure, ...). Must poll isCancelled() periodically
    // and stop (throwing ErrorCategory::Cancelled) when it returns true, same as Download().
    virtual DownloadMetadata Inspect(const std::string& url, CancelledCallback isCancelled) = 0;

    // Blocking; intended to run on a job's worker thread. Throws
    // errors::MediaToolException on failure. Must poll isCancelled() periodically and
    // stop (throwing with ErrorCategory::Cancelled) when it returns true.
    virtual void Download(const DownloadOptions& options, MetadataCallback onMetadata,
                          ProgressCallback onProgress, CompletedCallback onCompleted,
                          CancelledCallback isCancelled) = 0;
};

}  // namespace mediatool::downloads
