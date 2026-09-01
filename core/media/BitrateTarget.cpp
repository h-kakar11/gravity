#include "core/media/BitrateTarget.h"

#include <cmath>
#include <unordered_set>

namespace mediatool::media {

namespace {

const std::unordered_set<std::string> kAudioOnlyFormats = {
    "mp3", "wav", "flac", "aac", "m4a", "ogg", "opus",
};

// Fraction of the source bitrate a given quality tier targets. Compression tiers are all
// well below 1.0 -- that is the whole point of the mode; conversion tiers straddle 1.0
// because a conversion only has to avoid ballooning, not shrink.
double BitrateFactorForQuality(const std::string& quality, bool isCompression) {
    if (isCompression) {
        if (quality == "lowest") return 0.20;
        if (quality == "low") return 0.30;
        if (quality == "high") return 0.60;
        if (quality == "ultra") return 0.75;
        return 0.45;  // medium, and the default for any unrecognized value
    }
    if (quality == "lowest") return 0.35;
    if (quality == "low") return 0.50;
    if (quality == "high") return 1.00;
    if (quality == "ultra") return 1.30;
    return 0.75;  // medium, and the default for any unrecognized value
}

}  // namespace

bool IsAudioOnlyOutputFormat(const std::string& outputFormat) {
    return kAudioOnlyFormats.count(outputFormat) > 0;
}

std::optional<int> TargetBitrateKbps(int sourceBitrateKbps, const std::string& quality,
                                      bool isCompression) {
    // "lossless" and a bitrate cap are contradictory instructions; honour the explicit one.
    if (quality == "lossless") return std::nullopt;
    // ffprobe reports no bitrate for some inputs (raw streams, some remuxed containers).
    // Guessing a target from a number we don't have would be worse than imposing none.
    if (sourceBitrateKbps <= 0) return std::nullopt;

    const double scaled =
        static_cast<double>(sourceBitrateKbps) * BitrateFactorForQuality(quality, isCompression);
    const int target = static_cast<int>(std::llround(scaled));
    return target < kMinTargetBitrateKbps ? kMinTargetBitrateKbps : target;
}

}  // namespace mediatool::media
