#include "engines/ffmpeg/FFmpegErrorClassifier.h"

#include <gtest/gtest.h>

#include <string>

namespace mediatool::media {
namespace {

using errors::ErrorCategory;

TEST(FFmpegErrorClassifier, DiskFullIsItsOwnErrorAndSaysHowMuchSpaceIsLeft) {
    // The one failure a user can fix immediately, and previously indistinguishable from
    // every other non-zero exit.
    const auto error = ClassifyFfmpegFailure(
        "[out#0/mp4 @ 0x55] Error writing trailer: No space left on device\n", 1,
        /*availableBytes=*/43'200'000);
    EXPECT_EQ(error.code, "E_DISK_FULL");
    EXPECT_EQ(error.category, ErrorCategory::DiskSpaceError);
    // Never retried: the disk will still be full a few seconds later. The retry policy
    // vetoes this category outright, and the flag agrees.
    EXPECT_FALSE(error.recoverable);
    EXPECT_NE(error.message.find("41.2 MB"), std::string::npos) << error.message;
}

TEST(FFmpegErrorClassifier, DiskFullWithoutASpaceReadingStillClassifiesCorrectly) {
    // The free-space figure is decoration on the message, never an input to the decision.
    const auto error =
        ClassifyFfmpegFailure("No space left on device\n", 1, /*availableBytes=*/std::nullopt);
    EXPECT_EQ(error.code, "E_DISK_FULL");
    EXPECT_EQ(error.message.find("free there"), std::string::npos);
}

TEST(FFmpegErrorClassifier, PermissionDeniedCarriesARecoveryHintNotJustACode) {
    // "Permission denied" alone leaves a user with nowhere to go. On Windows the usual
    // causes are a protected folder and antivirus, so the message names both.
    const auto error =
        ClassifyFfmpegFailure("Error opening output file: Permission denied\n", 1, std::nullopt);
    EXPECT_EQ(error.code, "E_PERMISSION_DENIED");
    EXPECT_EQ(error.category, ErrorCategory::PermissionError);
    EXPECT_NE(error.message.find("antivirus"), std::string::npos) << error.message;
}

TEST(FFmpegErrorClassifier, ACorruptFileIsAnInvalidFileNotAnEngineFailure) {
    // Real ffmpeg 6.1.1 output for a truncated mp4. The category matters as much as the
    // code: EngineFailure is retryable, InvalidFile is not, and re-running ffmpeg on the
    // same broken file forever helps nobody.
    const auto truncated = ClassifyFfmpegFailure(
        "[mov,mp4,m4a,3gp,3g2,mj2 @ 0x55] moov atom not found\n"
        "[in#0 @ 0x55] Error opening input: Invalid data found when processing input\n",
        1, std::nullopt);
    EXPECT_EQ(truncated.code, "E_INVALID_FILE");
    EXPECT_EQ(truncated.category, ErrorCategory::InvalidFile);
    EXPECT_FALSE(truncated.recoverable);
    // The more specific of the two matching lines wins: "its index is missing" tells the
    // user the download did not finish, which "not a media file" does not.
    EXPECT_NE(truncated.message.find("incomplete"), std::string::npos) << truncated.message;
}

TEST(FFmpegErrorClassifier, AMissingCodecIsReportedAsTheBuildsLimitationNotTheFilesFault) {
    const auto error = ClassifyFfmpegFailure(
        "[vost#0:0 @ 0x55] Unknown encoder 'definitelynotacodec'\n", 1, std::nullopt);
    EXPECT_EQ(error.code, "E_UNSUPPORTED_CODEC");
    EXPECT_EQ(error.category, ErrorCategory::UnsupportedFormat);
    EXPECT_NE(error.message.find("ffmpeg build"), std::string::npos) << error.message;
}

TEST(FFmpegErrorClassifier, AMissingInputFileIsNotAnEngineFailureEither) {
    const auto error = ClassifyFfmpegFailure(
        "[in#0 @ 0x55] Error opening input: No such file or directory\n", 1, std::nullopt);
    EXPECT_EQ(error.code, "E_FILE_NOT_FOUND");
    EXPECT_EQ(error.category, ErrorCategory::FileNotFound);
}

TEST(FFmpegErrorClassifier, AnUnrecognizedFailureKeepsTheGenericCodeAndTheEvidence) {
    // The table is a heuristic over ffmpeg's prose. Guessing at something it does not
    // recognize would be worse than the generic answer -- but the stderr has to survive,
    // because it is the only record of what actually went wrong.
    const auto error =
        ClassifyFfmpegFailure("some future ffmpeg message nobody has seen yet\n", 42, std::nullopt);
    EXPECT_EQ(error.code, "E_FFMPEG_FAILED");
    EXPECT_EQ(error.category, ErrorCategory::EngineFailure);
    EXPECT_NE(error.details.find("exitCode=42"), std::string::npos);
    EXPECT_NE(error.details.find("some future ffmpeg message"), std::string::npos);
}

TEST(FFmpegErrorClassifier, AnUnrecognizedProbeFailureMeansTheFileIsUnreadable) {
    // Different default for the same table: ffprobe being unable to read a file IS "this
    // is not a media file we can work with", and calling that an engine failure would put
    // it back in a retryable category.
    const auto error = ClassifyFfprobeFailure("something inscrutable\n", 1);
    EXPECT_EQ(error.code, "E_INVALID_FILE");
    EXPECT_EQ(error.category, ErrorCategory::InvalidFile);
    EXPECT_FALSE(error.recoverable);
}

TEST(FFmpegErrorClassifier, ProbeFailuresStillUseTheSpecificClassificationWhenThereIsOne) {
    const auto error =
        ClassifyFfprobeFailure("Error opening input: Permission denied\n", 1);
    EXPECT_EQ(error.code, "E_PERMISSION_DENIED");
    EXPECT_EQ(error.category, ErrorCategory::PermissionError);
}

TEST(FFmpegErrorClassifier, ClassificationIsCaseInsensitiveAndSurvivesSurroundingNoise) {
    // ffmpeg's casing varies by which component emits the line, and the real stderr tail
    // is twenty lines of progress spam with the failure somewhere inside it.
    const std::string noisy =
        "frame= 100 fps=0.0 q=28.0 size=  256kB time=00:00:04.00 bitrate= 524.3kbits/s\n"
        "frame= 200 fps=0.0 q=28.0 size=  512kB time=00:00:08.00 bitrate= 524.3kbits/s\n"
        "[out#0/mp4 @ 0x55] Could not write header: NO SPACE LEFT ON DEVICE\n"
        "Conversion failed!\n";
    const auto error = ClassifyFfmpegFailure(noisy, 1, std::nullopt);
    EXPECT_EQ(error.code, "E_DISK_FULL");
}

TEST(FFmpegErrorClassifier, ByteCountsAreRenderedForAHuman) {
    EXPECT_EQ(DescribeByteCount(0), "0 B");
    EXPECT_EQ(DescribeByteCount(512), "512 B");
    EXPECT_EQ(DescribeByteCount(1024), "1.0 KB");
    EXPECT_EQ(DescribeByteCount(1536), "1.5 KB");
    EXPECT_EQ(DescribeByteCount(43'200'000), "41.2 MB");
    EXPECT_EQ(DescribeByteCount(5ull * 1024 * 1024 * 1024), "5.0 GB");
}

}  // namespace
}  // namespace mediatool::media
