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
            return "bestaudio/best";
    }
    return "bestvideo*+bestaudio/best";
}

}  // namespace mediatool::downloader
