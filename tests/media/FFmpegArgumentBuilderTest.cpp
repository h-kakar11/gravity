// The argv every conversion and compression actually runs (spec section 16). Pure, so no
// ffmpeg process is involved -- which is the point: these assert the codec, container and
// filter decisions directly rather than inferring them from an encode's output.

#include "engines/ffmpeg/FFmpegArgumentBuilder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "core/errors/MediaToolException.h"
#include "core/media/ProcessingOptions.h"

using mediatool::errors::MediaToolException;
using mediatool::media::BuildCompressionArgs;
using mediatool::media::BuildConversionArgs;
using mediatool::media::CompressionPreset;
using mediatool::media::CompressionRequest;
using mediatool::media::ConversionRequest;
using mediatool::media::CrfForPreset;
using mediatool::media::TargetFormat;

namespace {

bool Contains(const std::vector<std::string>& args, const std::string& value) {
    return std::find(args.begin(), args.end(), value) != args.end();
}

// True if `flag` is immediately followed by `value` -- "contains -c:a and contains aac"
// would pass even if they belonged to different flags.
bool HasFlagValue(const std::vector<std::string>& args, const std::string& flag,
                  const std::string& value) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == flag && args[i + 1] == value) return true;
    }
    return false;
}

std::string ValueAfter(const std::vector<std::string>& args, const std::string& flag) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == flag) return args[i + 1];
    }
    return "";
}

ConversionRequest Conversion(TargetFormat format) {
    ConversionRequest request;
    request.targetFormat = format;
    return request;
}

}  // namespace

// --- the shared preamble ------------------------------------------------------------------

TEST(FFmpegArgumentBuilder, EveryInvocationCarriesTheSafetyPreamble) {
    const auto check = [](const std::vector<std::string>& args) {
        EXPECT_TRUE(Contains(args, "-hide_banner"));
        EXPECT_TRUE(Contains(args, "-y"));
        // Without -nostdin, ffmpeg inherits our stdin and fights the NDJSON protocol
        // reader for input.
        EXPECT_TRUE(Contains(args, "-nostdin"));
        // -progress pipe:1 is what FFmpegProgressParser consumes.
        EXPECT_TRUE(HasFlagValue(args, "-progress", "pipe:1"));
        EXPECT_TRUE(HasFlagValue(args, "-i", "in.mp4"));
        EXPECT_EQ(args.back(), "out.tmp");
    };

    check(BuildConversionArgs("in.mp4", "out.tmp", Conversion(TargetFormat::Mp4)));
    check(BuildCompressionArgs("in.mp4", "out.tmp", CompressionRequest{}, /*hasVideoStream=*/true));
}

TEST(FFmpegArgumentBuilder, InputComesBeforeOutput) {
    // ffmpeg is positional: an output path placed before -i silently becomes an input.
    const auto args = BuildConversionArgs("in.mp4", "out.tmp", Conversion(TargetFormat::Mkv));
    const auto inputIndex = std::find(args.begin(), args.end(), "in.mp4") - args.begin();
    const auto outputIndex = std::find(args.begin(), args.end(), "out.tmp") - args.begin();
    EXPECT_LT(inputIndex, outputIndex);
}

TEST(FFmpegArgumentBuilder, RejectsEmptyPaths) {
    EXPECT_THROW(BuildConversionArgs("", "out.tmp", Conversion(TargetFormat::Mp4)), MediaToolException);
    EXPECT_THROW(BuildConversionArgs("in.mp4", "", Conversion(TargetFormat::Mp4)), MediaToolException);
    EXPECT_THROW(BuildCompressionArgs("", "out.tmp", CompressionRequest{}, true), MediaToolException);
}

// --- video conversion ------------------------------------------------------------------------

TEST(FFmpegArgumentBuilder, Mp4AndMovUseH264AndMoveTheIndexToTheFront) {
    for (const auto format : {TargetFormat::Mp4, TargetFormat::Mov}) {
        const auto args = BuildConversionArgs("in.mkv", "out.tmp", Conversion(format));
        EXPECT_TRUE(HasFlagValue(args, "-c:v", "libx264"));
        EXPECT_TRUE(HasFlagValue(args, "-c:a", "aac"));
        // Standard for these containers: the result starts playing before it is fully
        // copied.
        EXPECT_TRUE(HasFlagValue(args, "-movflags", "+faststart"));
        EXPECT_TRUE(HasFlagValue(args, "-pix_fmt", "yuv420p"));
    }
}

TEST(FFmpegArgumentBuilder, MkvDoesNotGetFaststart) {
    // +faststart is an MP4/MOV atom concept; Matroska has no equivalent.
    const auto args = BuildConversionArgs("in.mp4", "out.tmp", Conversion(TargetFormat::Mkv));
    EXPECT_FALSE(Contains(args, "-movflags"));
    EXPECT_TRUE(HasFlagValue(args, "-c:v", "libx264"));
}

TEST(FFmpegArgumentBuilder, WebMUsesVp9AndOpusWithConstantQuality) {
    const auto args = BuildConversionArgs("in.mp4", "out.tmp", Conversion(TargetFormat::WebM));
    EXPECT_TRUE(HasFlagValue(args, "-c:v", "libvpx-vp9"));
    EXPECT_TRUE(HasFlagValue(args, "-c:a", "libopus"));
    // VP9 ignores -crf entirely unless -b:v is 0. Getting this wrong produces a file that
    // encodes fine and looks nothing like the requested quality.
    EXPECT_TRUE(HasFlagValue(args, "-b:v", "0"));
}

// --- audio-only conversion ---------------------------------------------------------------------

TEST(FFmpegArgumentBuilder, AudioTargetsDropVideoAndPickTheRightEncoder) {
    struct Case {
        TargetFormat format;
        std::string codec;
    };
    const Case cases[] = {
        {TargetFormat::Mp3, "libmp3lame"},
        {TargetFormat::M4a, "aac"},
        {TargetFormat::Opus, "libopus"},
        {TargetFormat::Wav, "pcm_s16le"},
        {TargetFormat::Flac, "flac"},
    };
    for (const auto& [format, codec] : cases) {
        const auto args = BuildConversionArgs("in.mp4", "out.tmp", Conversion(format));
        // -vn matters: without it a cover-art stream becomes a video stream ffmpeg then
        // tries to encode into an audio container.
        EXPECT_TRUE(Contains(args, "-vn")) << codec;
        EXPECT_TRUE(HasFlagValue(args, "-c:a", codec)) << codec;
        EXPECT_FALSE(Contains(args, "-c:v")) << codec;
    }
}

TEST(FFmpegArgumentBuilder, LosslessAudioTargetsIgnoreABitrate) {
    // A bitrate flag is meaningless for PCM or FLAC, so it is not emitted even when asked.
    for (const auto format : {TargetFormat::Wav, TargetFormat::Flac}) {
        ConversionRequest request = Conversion(format);
        request.audioBitrateKbps = 320;
        const auto args = BuildConversionArgs("in.mp4", "out.tmp", request);
        EXPECT_FALSE(Contains(args, "-b:a"));
    }
}

TEST(FFmpegArgumentBuilder, AudioBitrateIsHonouredForLossyTargets) {
    ConversionRequest request = Conversion(TargetFormat::Mp3);
    request.audioBitrateKbps = 320;
    EXPECT_TRUE(HasFlagValue(BuildConversionArgs("in.mp4", "out.tmp", request), "-b:a", "320k"));
}

TEST(FFmpegArgumentBuilder, VideoOnlyOptionsAreRejectedForAudioTargets) {
    // Silently ignoring these would produce output the user did not ask for.
    ConversionRequest request = Conversion(TargetFormat::Mp3);
    request.maxHeight = 720;
    EXPECT_THROW(BuildConversionArgs("in.mp4", "out.tmp", request), MediaToolException);

    ConversionRequest withFps = Conversion(TargetFormat::Wav);
    withFps.gifFps = 12;
    EXPECT_THROW(BuildConversionArgs("in.mp4", "out.tmp", withFps), MediaToolException);
}

TEST(FFmpegArgumentBuilder, AFrameRateOptionOnlyAppliesToGif) {
    ConversionRequest request = Conversion(TargetFormat::Mp4);
    request.gifFps = 12;
    EXPECT_THROW(BuildConversionArgs("in.mp4", "out.tmp", request), MediaToolException);
}

// --- GIF -----------------------------------------------------------------------------------

TEST(FFmpegArgumentBuilder, GifGeneratesAPaletteInASinglePass) {
    ConversionRequest request = Conversion(TargetFormat::Gif);
    request.gifFps = 15;
    request.maxHeight = 240;
    const auto args = BuildConversionArgs("in.mp4", "out.tmp", request);

    const std::string filter = ValueAfter(args, "-filter_complex");
    // Without palettegen/paletteuse a GIF is quantized to a generic 256-colour palette and
    // looks terrible; the split graph does both in one invocation.
    EXPECT_NE(filter.find("palettegen"), std::string::npos);
    EXPECT_NE(filter.find("paletteuse"), std::string::npos);
    EXPECT_NE(filter.find("fps=15"), std::string::npos);
    EXPECT_NE(filter.find("240"), std::string::npos);
    EXPECT_TRUE(Contains(args, "-an"));  // GIF carries no audio
    EXPECT_TRUE(HasFlagValue(args, "-loop", "0"));
}

TEST(FFmpegArgumentBuilder, GifHasADefaultFrameRate) {
    const auto args = BuildConversionArgs("in.mp4", "out.tmp", Conversion(TargetFormat::Gif));
    EXPECT_NE(ValueAfter(args, "-filter_complex").find("fps="), std::string::npos);
}

// --- scaling --------------------------------------------------------------------------------

TEST(FFmpegArgumentBuilder, ScalingRoundsToEvenDimensionsAndNeverUpscales) {
    ConversionRequest request = Conversion(TargetFormat::Mp4);
    request.maxHeight = 720;
    const std::string filter = ValueAfter(BuildConversionArgs("in.mp4", "out.tmp", request), "-vf");

    // -2 rather than -1: x264 requires even dimensions and would fail outright on an odd
    // computed width.
    EXPECT_NE(filter.find("scale=-2"), std::string::npos);
    // min(...) keeps the filter a no-op for a source already shorter than the cap, rather
    // than upscaling and growing the file the user asked to shrink.
    EXPECT_NE(filter.find("min(720,ih)"), std::string::npos);
}

TEST(FFmpegArgumentBuilder, NoScaleFilterWhenNoHeightIsRequested) {
    EXPECT_FALSE(Contains(BuildConversionArgs("in.mp4", "out.tmp", Conversion(TargetFormat::Mp4)), "-vf"));
}

// --- compression -----------------------------------------------------------------------------

TEST(FFmpegArgumentBuilder, CompressionPresetsMapToDistinctCrfValues) {
    // Lower CRF = higher quality = bigger file. If two presets collided, the UI would be
    // offering a choice that does nothing.
    EXPECT_GT(CrfForPreset(CompressionPreset::Low), CrfForPreset(CompressionPreset::Medium));
    EXPECT_GT(CrfForPreset(CompressionPreset::Medium), CrfForPreset(CompressionPreset::High));

    for (const auto preset : {CompressionPreset::Low, CompressionPreset::Medium, CompressionPreset::High}) {
        CompressionRequest request;
        request.preset = preset;
        const auto args = BuildCompressionArgs("in.mp4", "out.tmp", request, true);
        EXPECT_TRUE(HasFlagValue(args, "-crf", std::to_string(CrfForPreset(preset))));
    }
}

TEST(FFmpegArgumentBuilder, CompressionReEncodesVideoAndAudio) {
    const auto args = BuildCompressionArgs("in.mp4", "out.tmp", CompressionRequest{}, true);
    EXPECT_TRUE(HasFlagValue(args, "-c:v", "libx264"));
    EXPECT_TRUE(HasFlagValue(args, "-c:a", "aac"));
    EXPECT_TRUE(Contains(args, "-b:a"));
}

TEST(FFmpegArgumentBuilder, AudioOnlyInputIsCompressedWithoutAVideoEncoder) {
    // Running a video encoder over an audio-only file just fails with "no video stream".
    const auto args = BuildCompressionArgs("in.mp3", "out.tmp", CompressionRequest{}, /*hasVideoStream=*/false);
    EXPECT_TRUE(Contains(args, "-vn"));
    EXPECT_FALSE(Contains(args, "-c:v"));
    EXPECT_FALSE(Contains(args, "-crf"));
    EXPECT_TRUE(HasFlagValue(args, "-c:a", "aac"));
}

TEST(FFmpegArgumentBuilder, ResizingAnAudioOnlyFileIsRejected) {
    CompressionRequest request;
    request.maxHeight = 480;
    EXPECT_THROW(BuildCompressionArgs("in.mp3", "out.tmp", request, /*hasVideoStream=*/false),
                 MediaToolException);
}

TEST(FFmpegArgumentBuilder, CompressionHonoursAnExplicitAudioBitrate) {
    CompressionRequest request;
    request.audioBitrateKbps = 96;
    EXPECT_TRUE(
        HasFlagValue(BuildCompressionArgs("in.mp4", "out.tmp", request, true), "-b:a", "96k"));
}

// --- option parsing --------------------------------------------------------------------------

TEST(ProcessingOptions, TargetFormatRoundTripsThroughItsWireString) {
    using mediatool::media::TargetFormatFromWireString;
    using mediatool::media::ToWireString;
    for (const auto& wire : mediatool::media::AllTargetFormatWireStrings()) {
        EXPECT_EQ(ToWireString(TargetFormatFromWireString(wire)), wire);
    }
}

TEST(ProcessingOptions, EveryFormatHasAnExtensionAndAKnownAudioOnlyFlag) {
    using mediatool::media::ExtensionFor;
    using mediatool::media::IsAudioOnly;
    using mediatool::media::TargetFormatFromWireString;
    for (const auto& wire : mediatool::media::AllTargetFormatWireStrings()) {
        const auto format = TargetFormatFromWireString(wire);
        EXPECT_FALSE(ExtensionFor(format).empty()) << wire;
        // GIF is video-ish but audio-free; the flag means "drops the video stream", so it
        // must be false for GIF.
        if (wire == "GIF") EXPECT_FALSE(IsAudioOnly(format));
    }
}

TEST(ProcessingOptions, AnUnknownFormatOrPresetIsRejectedRatherThanDefaulted) {
    using mediatool::media::CompressionPresetFromWireString;
    using mediatool::media::TargetFormatFromWireString;
    EXPECT_THROW(TargetFormatFromWireString("REALMEDIA"), MediaToolException);
    EXPECT_THROW(TargetFormatFromWireString("mp4"), MediaToolException);  // wire form is upper-case
    EXPECT_THROW(CompressionPresetFromWireString("EXTREME"), MediaToolException);
}

TEST(ProcessingOptions, ConversionRequestRequiresATargetFormat) {
    EXPECT_THROW(ConversionRequest::FromJson(nlohmann::json::object()), MediaToolException);
    EXPECT_THROW(ConversionRequest::FromJson({{"targetFormat", 42}}), MediaToolException);
    EXPECT_THROW(ConversionRequest::FromJson(nlohmann::json::array()), MediaToolException);
}

TEST(ProcessingOptions, OutOfRangeNumbersAreRejectedRatherThanClamped) {
    // Clamping would produce output the user did not ask for (spec section 54).
    EXPECT_THROW(ConversionRequest::FromJson({{"targetFormat", "MP4"}, {"maxHeight", 999999}}),
                 MediaToolException);
    EXPECT_THROW(ConversionRequest::FromJson({{"targetFormat", "MP4"}, {"maxHeight", 2}}),
                 MediaToolException);
    EXPECT_THROW(ConversionRequest::FromJson({{"targetFormat", "GIF"}, {"gifFps", 0}}),
                 MediaToolException);
    EXPECT_THROW(ConversionRequest::FromJson({{"targetFormat", "MP3"}, {"audioBitrateKbps", 99999}}),
                 MediaToolException);
    EXPECT_THROW(ConversionRequest::FromJson({{"targetFormat", "MP4"}, {"maxHeight", 720.5}}),
                 MediaToolException);
}

TEST(ProcessingOptions, RequestsRoundTripThroughJson) {
    ConversionRequest conversion;
    conversion.targetFormat = TargetFormat::WebM;
    conversion.maxHeight = 480;
    conversion.audioBitrateKbps = 128;
    const auto restoredConversion = ConversionRequest::FromJson(conversion.ToJson());
    EXPECT_EQ(restoredConversion.targetFormat, TargetFormat::WebM);
    EXPECT_EQ(restoredConversion.maxHeight, 480);
    EXPECT_EQ(restoredConversion.audioBitrateKbps, 128);

    CompressionRequest compression;
    compression.preset = CompressionPreset::High;
    compression.maxHeight = 1080;
    const auto restoredCompression = CompressionRequest::FromJson(compression.ToJson());
    EXPECT_EQ(restoredCompression.preset, CompressionPreset::High);
    EXPECT_EQ(restoredCompression.maxHeight, 1080);
}

TEST(ProcessingOptions, CompressionDefaultsToMediumWhenNoPresetIsGiven) {
    EXPECT_EQ(CompressionRequest::FromJson(nlohmann::json::object()).preset, CompressionPreset::Medium);
}
