#include "engines/downloader/YtDlpFormatSelector.h"

namespace mediatool::downloader {

using downloads::QualityPreset;

std::string FormatSelectorForQuality(QualityPreset preset) {
    switch (preset) {
        case QualityPreset::Best:
            return "bestvideo*+bestaudio/best";
        case QualityPreset::P2160:
            return "bestvideo[height<=2160]+bestaudio/best[height<=2160]";
        case QualityPreset::P1440:
            return "bestvideo[height<=1440]+bestaudio/best[height<=1440]";
        case QualityPreset::P1080:
            return "bestvideo[height<=1080]+bestaudio/best[height<=1080]";
        case QualityPreset::P720:
            return "bestvideo[height<=720]+bestaudio/best[height<=720]";
        case QualityPreset::P480:
            return "bestvideo[height<=480]+bestaudio/best[height<=480]";
        case QualityPreset::AudioOnly:
            // No "/best" fallback here on purpose (issue #31): that fallback means "best
            // combined format if no pure-audio format exists," which can silently hand
            // back a full video file for an "Audio only" request. yt-dlp fails clearly
            // with E_FORMAT_UNAVAILABLE if no audio-only stream exists at all, which is
            // the correct behavior for this preset -- matches the existing
            // AudioOnlyNeverSelectsVideo test's intent (its literal assertion, "selector
            // doesn't contain the substring 'video'", already passed with "bestaudio/best"
            // too, since "/best" itself doesn't fail that check -- this closes the gap
            // between what the test asserts and what it's actually named for).
            return "bestaudio";
    }
    return "bestvideo*+bestaudio/best";
}

}  // namespace mediatool::downloader
