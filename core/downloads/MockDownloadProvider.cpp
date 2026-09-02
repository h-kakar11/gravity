#include "core/downloads/MockDownloadProvider.h"

#include "core/errors/MediaToolException.h"

namespace mediatool::downloads {

namespace {

[[noreturn]] void ThrowCancelled(const char* code) {
    throw errors::MediaToolException(errors::ErrorInfo::Make(
        code, errors::ErrorCategory::Cancelled, "Operation was cancelled.", "", /*recoverable=*/true));
}

}  // namespace

bool MockDownloadProvider::CanHandle(const std::string&) const { return true; }

DownloadMetadata MockDownloadProvider::Inspect(const std::string& url, CancelledCallback isCancelled) {
    lastInspectedUrl = url;
    if (respectCancellation && isCancelled && isCancelled()) {
        ThrowCancelled("E_INSPECT_CANCELLED");
    }
    if (inspectError) {
        throw errors::MediaToolException(*inspectError);
    }
    return inspectResult;
}

PlaylistInfo MockDownloadProvider::InspectPlaylist(const std::string& url,
                                                    CancelledCallback isCancelled) {
    lastInspectedPlaylistUrl = url;
    if (respectCancellation && isCancelled && isCancelled()) {
        ThrowCancelled("E_INSPECT_PLAYLIST_CANCELLED");
    }
    if (inspectPlaylistError) {
        throw errors::MediaToolException(*inspectPlaylistError);
    }
    return playlistResult;
}

void MockDownloadProvider::Download(const DownloadOptions& options, MetadataCallback onMetadata,
                                     ProgressCallback onProgress, CompletedCallback onCompleted,
                                     CancelledCallback isCancelled) {
    lastDownloadOptions = options;
    if (onDownloadStart) onDownloadStart(options);
    if (onMetadata) onMetadata(inspectResult);

    for (const auto& progress : progressSequence) {
        if (respectCancellation && isCancelled && isCancelled()) {
            ThrowCancelled("E_DOWNLOAD_CANCELLED");
        }
        if (onProgress) onProgress(progress);
    }

    if (respectCancellation && isCancelled && isCancelled()) {
        ThrowCancelled("E_DOWNLOAD_CANCELLED");
    }

    if (downloadError) {
        throw errors::MediaToolException(*downloadError);
    }

    if (onCompleted) onCompleted(completedOutputPath);
}

}  // namespace mediatool::downloads
