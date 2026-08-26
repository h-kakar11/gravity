#include "engines/ffmpeg/FFmpegArgBuilder.h"

#include <gtest/gtest.h>

#include <algorithm>

using mediatool::media::BuildFfmpegArgs;
using mediatool::media::MediaProcessingOptions;
using mediatool::media::WatermarkOptions;

namespace {

bool Contains(const std::vector<std::string>& args, const std::string& value) {
    return std::find(args.begin(), args.end(), value) != args.end();
}

// Returns the value immediately following the first occurrence of `flag`, or "" if not found.
std::string ValueAfter(const std::vector<std::string>& args, const std::string& flag) {
    auto it = std::find(args.begin(), args.end(), flag);
    if (it == args.end() || std::next(it) == args.end()) return "";
    return *std::next(it);
}

MediaProcessingOptions BasicVideoOptions() {
    MediaProcessingOptions options;
    options.outputFormat = "mp4";
    return options;
}

}  // namespace

TEST(FFmpegArgBuilderTest, ThrowsWhenOutputFormatMissing) {
    MediaProcessingOptions options;
    EXPECT_THROW(BuildFfmpegArgs("in.mov", "out.mp4", options, {}), std::exception);
}

TEST(FFmpegArgBuilderTest, DefaultsToBundledOpenH264WhenUnprobed) {
    // Empty availableEncoders == "not probed" -- must default to the LGPL-safe bundled
    // encoder, never assume libx264 is present (that would bundle a GPL codec).
    auto args = BuildFfmpegArgs("in.mov", "out.mp4", BasicVideoOptions(), {});
    EXPECT_EQ(ValueAfter(args, "-c:v"), "libopenh264");
}

TEST(FFmpegArgBuilderTest, UsesLibx264WhenTheResolvedFfmpegReportsIt) {
    // A user-supplied ffmpeg override that happens to have libx264 -- Gravity itself
    // still isn't bundling the GPL code, but using what's already there is fine.
    auto args = BuildFfmpegArgs("in.mov", "out.mp4", BasicVideoOptions(), {"libx264", "aac"});
    EXPECT_EQ(ValueAfter(args, "-c:v"), "libx264");
}

TEST(FFmpegArgBuilderTest, QualityTiersMapToExpectedCrfForH264) {
    MediaProcessingOptions low = BasicVideoOptions();
    low.quality = "low";
    EXPECT_EQ(ValueAfter(BuildFfmpegArgs("in.mp4", "out.mp4", low, {}), "-crf"), "28");

    MediaProcessingOptions medium = BasicVideoOptions();
    medium.quality = "medium";
    EXPECT_EQ(ValueAfter(BuildFfmpegArgs("in.mp4", "out.mp4", medium, {}), "-crf"), "23");

    MediaProcessingOptions high = BasicVideoOptions();
    high.quality = "high";
    EXPECT_EQ(ValueAfter(BuildFfmpegArgs("in.mp4", "out.mp4", high, {}), "-crf"), "18");
}

TEST(FFmpegArgBuilderTest, Vp9UsesAWiderCrfRangeThanH264) {
    MediaProcessingOptions options;
    options.outputFormat = "webm";
    options.videoCodec = "vp9";
    options.quality = "medium";
    auto args = BuildFfmpegArgs("in.mp4", "out.webm", options, {});
    EXPECT_EQ(ValueAfter(args, "-c:v"), "libvpx-vp9");
    EXPECT_EQ(ValueAfter(args, "-crf"), "31");
}

TEST(FFmpegArgBuilderTest, HardwareAccelerationAutoPicksFirstAvailableEncoder) {
    MediaProcessingOptions options = BasicVideoOptions();
    options.hardwareAcceleration = "auto";
    auto args = BuildFfmpegArgs("in.mp4", "out.mp4", options, {"h264_amf", "libopenh264"});
    EXPECT_EQ(ValueAfter(args, "-c:v"), "h264_amf");
    // Hardware encoders don't get a -crf flag -- they use their own internal quality mode.
    EXPECT_FALSE(Contains(args, "-crf"));
}

TEST(FFmpegArgBuilderTest, SpecificHardwareAccelerationRequestFallsBackToSoftwareWhenUnavailable) {
    MediaProcessingOptions options = BasicVideoOptions();
    options.hardwareAcceleration = "nvenc";
    auto args = BuildFfmpegArgs("in.mp4", "out.mp4", options, {});  // nvenc not in the set
    EXPECT_EQ(ValueAfter(args, "-c:v"), "libopenh264");
}

TEST(FFmpegArgBuilderTest, TrimAddsSsAndToBeforeInput) {
    MediaProcessingOptions options = BasicVideoOptions();
    options.trimStartSeconds = 5.0;
    options.trimEndSeconds = 15.5;
    auto args = BuildFfmpegArgs("in.mp4", "out.mp4", options, {});

    auto ssIt = std::find(args.begin(), args.end(), "-ss");
    auto inputIt = std::find(args.begin(), args.end(), "-i");
    ASSERT_NE(ssIt, args.end());
    ASSERT_NE(inputIt, args.end());
    EXPECT_LT(ssIt - args.begin(), inputIt - args.begin());  // -ss before -i: fast seek
    EXPECT_EQ(ValueAfter(args, "-to"), "15.500");
}

TEST(FFmpegArgBuilderTest, WatermarkAddsSecondInputAndFilterComplex) {
    MediaProcessingOptions options = BasicVideoOptions();
    WatermarkOptions watermark;
    watermark.imagePath = "logo.png";
    watermark.position = "top-left";
    watermark.opacity = 0.5;
    options.watermark = watermark;

    auto args = BuildFfmpegArgs("in.mp4", "out.mp4", options, {});
    EXPECT_TRUE(Contains(args, "-filter_complex"));
    EXPECT_EQ(ValueAfter(args, "-map"), "[vout]");

    const std::string filter = ValueAfter(args, "-filter_complex");
    EXPECT_NE(filter.find("overlay=10:10"), std::string::npos);  // top-left position
    EXPECT_NE(filter.find("colorchannelmixer=aa=0.500"), std::string::npos);

    // logo.png must appear as a second -i, after the primary input.
    int inputCount = 0;
    for (const auto& arg : args) {
        if (arg == "-i") ++inputCount;
    }
    EXPECT_EQ(inputCount, 2);
}

TEST(FFmpegArgBuilderTest, GifOutputUsesPaletteFilterPipelineNotNaiveEncoding) {
    MediaProcessingOptions options;
    options.outputFormat = "gif";
    auto args = BuildFfmpegArgs("in.mp4", "out.gif", options, {});

    EXPECT_TRUE(Contains(args, "-filter_complex"));
    const std::string filter = ValueAfter(args, "-filter_complex");
    EXPECT_NE(filter.find("palettegen"), std::string::npos);
    EXPECT_NE(filter.find("paletteuse"), std::string::npos);
    // Never a naive "-c:v gif" -- the whole point of this branch.
    EXPECT_FALSE(Contains(args, "gif"));
}

TEST(FFmpegArgBuilderTest, AudioOnlyFormatDropsVideoAndPicksMatchingEncoder) {
    MediaProcessingOptions options;
    options.outputFormat = "mp3";
    options.audioBitrateKbps = 192;
    auto args = BuildFfmpegArgs("in.mp4", "out.mp3", options, {});

    EXPECT_TRUE(Contains(args, "-vn"));
    EXPECT_EQ(ValueAfter(args, "-c:a"), "libmp3lame");
    EXPECT_EQ(ValueAfter(args, "-b:a"), "192k");
    EXPECT_FALSE(Contains(args, "-c:v"));
}

TEST(FFmpegArgBuilderTest, ImageFormatSkipsVideoEncoderAndAudioEntirely) {
    MediaProcessingOptions options;
    options.outputFormat = "webp";
    options.quality = "high";
    auto args = BuildFfmpegArgs("in.jpg", "out.webp", options, {});

    EXPECT_FALSE(Contains(args, "-c:v"));
    EXPECT_FALSE(Contains(args, "-c:a"));
    EXPECT_FALSE(Contains(args, "-crf"));
    EXPECT_EQ(ValueAfter(args, "-quality"), "95");
}

TEST(FFmpegArgBuilderTest, JpegQualityFlagIsInvertedFromEveryOtherQualityKnob) {
    MediaProcessingOptions high;
    high.outputFormat = "jpg";
    high.quality = "high";
    EXPECT_EQ(ValueAfter(BuildFfmpegArgs("in.png", "out.jpg", high, {}), "-q:v"), "2");  // lower == better

    MediaProcessingOptions low;
    low.outputFormat = "jpg";
    low.quality = "low";
    EXPECT_EQ(ValueAfter(BuildFfmpegArgs("in.png", "out.jpg", low, {}), "-q:v"), "12");
}

TEST(FFmpegArgBuilderTest, ResolutionAddsScaleFilterWhenNoWatermark) {
    MediaProcessingOptions options = BasicVideoOptions();
    options.resolutionWidth = 1280;
    options.resolutionHeight = 720;
    auto args = BuildFfmpegArgs("in.mp4", "out.mp4", options, {});
    EXPECT_EQ(ValueAfter(args, "-vf"), "scale=1280:720");
}

TEST(FFmpegArgBuilderTest, EveryVideoTargetIncludesProgressPipeForConsumptionByTheParser) {
    auto args = BuildFfmpegArgs("in.mp4", "out.mp4", BasicVideoOptions(), {});
    EXPECT_EQ(ValueAfter(args, "-progress"), "pipe:1");

    MediaProcessingOptions audio;
    audio.outputFormat = "mp3";
    EXPECT_EQ(ValueAfter(BuildFfmpegArgs("in.mp4", "out.mp3", audio, {}), "-progress"), "pipe:1");

    MediaProcessingOptions gif;
    gif.outputFormat = "gif";
    EXPECT_EQ(ValueAfter(BuildFfmpegArgs("in.mp4", "out.gif", gif, {}), "-progress"), "pipe:1");
}

TEST(FFmpegArgBuilderTest, LosslessMapsToCrfZeroIfItEverReachesThisLayer) {
    // The real Pro-tier gate lives at the IPC-handler layer (main.cpp rejects "lossless"
    // before a job is ever created) -- this only confirms BuildFfmpegArgs itself stays
    // independently correct/testable rather than assuming that gate already ran.
    MediaProcessingOptions options = BasicVideoOptions();
    options.quality = "lossless";
    EXPECT_EQ(ValueAfter(BuildFfmpegArgs("in.mp4", "out.mp4", options, {}), "-crf"), "0");
}
