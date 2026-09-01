#include "core/media/BitrateTarget.h"

#include <gtest/gtest.h>

#include <string>

using mediatool::media::kMinTargetBitrateKbps;
using mediatool::media::IsAudioOnlyOutputFormat;
using mediatool::media::TargetBitrateKbps;

// Issue #80: "compress" had no size objective at all -- it was the same fixed-CRF ffmpeg
// invocation as "convert", with no reference to the input's size. These cover the policy
// that replaced it, with no ffmpeg and no job machinery in the way.

TEST(BitrateTargetTest, CompressionTargetsAFractionOfTheSourceBitrate) {
    // Every compression tier must land BELOW the source, monotonically. This is the
    // property issue #80 was missing: "compress" previously had no size objective at all,
    // so a 1.19 MB source came back at 1.03x (medium) and 1.41x (high) its original size.
    const int source = 1000;
    int previous = 0;
    for (const std::string quality : {"lowest", "low", "medium", "high", "ultra"}) {
        const auto target = TargetBitrateKbps(source, quality, /*isCompression=*/true);
        ASSERT_TRUE(target.has_value()) << "quality=" << quality;
        EXPECT_LT(*target, source) << "quality=" << quality;
        EXPECT_GT(*target, previous) << "quality=" << quality;
        previous = *target;
    }
}

TEST(BitrateTargetTest, ConversionTargetsAreNotCompressionTargets) {
    // A conversion is not asked to shrink anything -- only to avoid ballooning -- so its
    // factors sit around 1.0 while compression's stay well under it.
    for (const std::string quality : {"lowest", "low", "medium", "high", "ultra"}) {
        const auto compress = TargetBitrateKbps(1000, quality, /*isCompression=*/true);
        const auto convert = TargetBitrateKbps(1000, quality, /*isCompression=*/false);
        ASSERT_TRUE(compress.has_value() && convert.has_value());
        EXPECT_LT(*compress, *convert) << "quality=" << quality;
    }
}

TEST(BitrateTargetTest, NoBitrateTargetIsInventedFromAnUnknownSource) {
    // ffprobe reports no bitrate for some inputs. Scaling a number we don't have would
    // produce a confidently wrong target; imposing none leaves prior behavior intact.
    EXPECT_FALSE(TargetBitrateKbps(0, "medium", true).has_value());
    EXPECT_FALSE(TargetBitrateKbps(-1, "medium", true).has_value());
    // "lossless" and a bitrate cap are contradictory instructions.
    EXPECT_FALSE(TargetBitrateKbps(1000, "lossless", false).has_value());
}

TEST(BitrateTargetTest, ATinySourceStillGetsAUsableBitrateFloor) {
    const auto target = TargetBitrateKbps(10, "lowest", /*isCompression=*/true);
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(*target, kMinTargetBitrateKbps);
}

TEST(BitrateTargetTest, AudioOnlyFormatsAreRecognizedForSizingPurposes) {
    // An mp3/wav target has no video stream for a video bitrate to apply to.
    EXPECT_TRUE(IsAudioOnlyOutputFormat("mp3"));
    EXPECT_TRUE(IsAudioOnlyOutputFormat("wav"));
    EXPECT_TRUE(IsAudioOnlyOutputFormat("flac"));
    EXPECT_FALSE(IsAudioOnlyOutputFormat("mp4"));
    EXPECT_FALSE(IsAudioOnlyOutputFormat("gif"));
    EXPECT_FALSE(IsAudioOnlyOutputFormat("png"));
}
