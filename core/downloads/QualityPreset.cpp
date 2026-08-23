#include "core/downloads/QualityPreset.h"

#include <stdexcept>

namespace mediatool::downloads {

std::string ToWireString(QualityPreset preset) {
    switch (preset) {
        case QualityPreset::Best: return "BEST";
        case QualityPreset::P2160: return "2160P";
        case QualityPreset::P1440: return "1440P";
        case QualityPreset::P1080: return "1080P";
        case QualityPreset::P720: return "720P";
        case QualityPreset::P480: return "480P";
        case QualityPreset::AudioOnly: return "AUDIO_ONLY";
    }
    throw std::invalid_argument("Unrecognized QualityPreset enum value");
}

QualityPreset QualityPresetFromWireString(const std::string& wire) {
    if (wire == "BEST") return QualityPreset::Best;
    if (wire == "2160P") return QualityPreset::P2160;
    if (wire == "1440P") return QualityPreset::P1440;
    if (wire == "1080P") return QualityPreset::P1080;
    if (wire == "720P") return QualityPreset::P720;
    if (wire == "480P") return QualityPreset::P480;
    if (wire == "AUDIO_ONLY") return QualityPreset::AudioOnly;
    throw std::invalid_argument("Unrecognized QualityPreset wire string: " + wire);
}

}  // namespace mediatool::downloads
