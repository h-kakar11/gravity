#pragma once

// Scripted, no-process IDownloadProvider for unit tests (spec section 37, spec section
// 39: "do not require a real network connection for unit tests"). DownloadJob tests use
// this instead of a real YtDlpProvider/Python/yt-dlp.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/downloads/IDownloadProvider.h"
#include "core/errors/ErrorInfo.h"

namespace mediatool::downloads {

class MockDownloadProvider : public IDownloadProvider {
public:
    bool CanHandle(const std::string& url) const override;

    // Scriptable, like everything else here; defaults to a healthy modern backend so a
    // test that does not care about downloader health does not have to say so.
    DownloaderInfo Info() override { return info; }

    DownloadMetadata Inspect(const std::string& url, CancelledCallback isCancelled) override;

    PlaylistInfo InspectPlaylist(const std::string& url, CancelledCallback isCancelled) override;

    void Download(const DownloadOptions& options, MetadataCallback onMetadata,
                  ProgressCallback onProgress, CompletedCallback onCompleted,
                  CancelledCallback isCancelled) override;

    // --- scripting -----------------------------------------------------------------
    DownloaderInfo info{/*available=*/true, "yt-dlp", std::string("2026.08.19"), 0, false};
    // Returned by Inspect(), and replayed to Download()'s onMetadata callback.
    DownloadMetadata inspectResult;
    std::optional<errors::ErrorInfo> inspectError;  // if set, Inspect() throws this instead

    PlaylistInfo playlistResult;                            // returned by InspectPlaylist()
    std::optional<errors::ErrorInfo> inspectPlaylistError;  // ...unless set, then it throws

    std::vector<jobs::Progress> progressSequence;    // replayed in order during Download()
    std::string completedOutputPath;                 // onCompleted() is called with this...
    std::optional<errors::ErrorInfo> downloadError;  // ...unless set, then Download() throws
    // If true (the default), isCancelled() is polled between each queued progress event
    // and Download() throws ErrorCategory::Cancelled the moment it returns true.
    bool respectCancellation = true;
    // Invoked as the first thing Download() does, with the exact DownloadOptions it was
    // called with -- lets a test simulate a filesystem side effect (e.g. the output file
    // "appearing") at the moment the download actually starts, which is after
    // DownloadJob has already computed filenameBase via DeduplicateBaseName. Without this
    // hook a test that pre-registers the eventual output file would make it visible to
    // that earlier dedup check too, which a real download never would.
    std::function<void(const DownloadOptions&)> onDownloadStart;

    // --- observation -----------------------------------------------------------------
    std::optional<DownloadOptions> lastDownloadOptions;
    std::string lastInspectedUrl;
    std::string lastInspectedPlaylistUrl;
};

}  // namespace mediatool::downloads
