#include "engines/downloader/YtDlpFormatSelector.h"

#include <cctype>

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

namespace {

constexpr std::size_t kMaxFormatIdLength = 64;
constexpr std::size_t kMaxFormatIdsPerSelector = 8;

// The two yt-dlp selector keywords that survive the grammar below (they are plain
// alphanumeric words) AND change how many streams get downloaded. "all" selects every
// format on the page and "mergeall" merges every one of them into a single file, so
// either one turns "download the stream I picked" into something the user never asked
// for. The other bare keywords -- best/worst/bestvideo/bestaudio/b/w/bv/ba and friends --
// are deliberately NOT blocked: they still resolve to one stream of the same video, and a
// site is free to name a real format id after one of them.
bool IsArityChangingKeyword(const std::string& token) {
    return token == "all" || token == "mergeall";
}

bool IsFormatIdCharacter(char c) {
    const auto uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) != 0 || c == '_' || c == '-' || c == '.';
}

}  // namespace

bool IsSafeFormatSelector(const std::string& selector) {
    if (selector.empty()) {
        return false;
    }

    std::size_t idCount = 0;
    std::string current;
    for (const char c : selector) {
        if (c == '+') {
            // Rejects a leading '+', a trailing '+' (`current` is still empty when the
            // loop ends, caught below) and "137++140".
            if (current.empty() || IsArityChangingKeyword(current)) {
                return false;
            }
            if (++idCount >= kMaxFormatIdsPerSelector) {
                return false;
            }
            current.clear();
            continue;
        }
        if (!IsFormatIdCharacter(c)) {
            return false;
        }
        current.push_back(c);
        if (current.size() > kMaxFormatIdLength) {
            return false;
        }
    }
    return !current.empty() && !IsArityChangingKeyword(current);
}

}  // namespace mediatool::downloader
