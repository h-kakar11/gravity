#pragma once

// Application-level download quality vocabulary (spec section 10). Everything above
// engines/downloader (DownloadJob, the IPC layer, the frontend) speaks only in terms of
// this enum -- see engines/downloader/YtDlpFormatSelector.h for the one place a
// QualityPreset becomes a concrete yt-dlp `-f` selector string. Keeping that translation
// out of this header is the point: nothing here should ever need to change if the
// concrete downloader implementation does.

#include <string>

namespace mediatool::downloads {

enum class QualityPreset {
    Best,
    P2160,
    P1440,
    P1080,
    P720,
    P480,
    AudioOnly,
};

// Wire strings per docs/ipc-contract.md: "BEST", "2160P", "1440P", "1080P", "720P",
// "480P", "AUDIO_ONLY". Throws std::invalid_argument on an unrecognized value/string.
std::string ToWireString(QualityPreset preset);
QualityPreset QualityPresetFromWireString(const std::string& wire);

}  // namespace mediatool::downloads
