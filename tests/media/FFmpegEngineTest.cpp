// Exercises FFmpegEngine against MockProcessRunner (core/process/MockProcessRunner.h) --
// no real ffmpeg/ffprobe process involved. Explicit override paths are used wherever a
// test needs to isolate a single IProcessRunner::Start() call (MockProcessRunner replays
// one canned response regardless of executable/args, so mixing an unresolved discovery
// call with an invocation call in the same test would conflate the two).

#include "engines/ffmpeg/FFmpegEngine.h"
#include "engines/ffmpeg/FFmpegDiscovery.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <gtest/gtest.h>

#include "core/errors/MediaToolException.h"
#include "core/process/MockProcessRunner.h"

using mediatool::errors::ErrorCategory;
using mediatool::errors::MediaToolException;
using mediatool::media::FFmpegEngine;
using mediatool::process::MockProcessRunner;

namespace {

// A real (but tiny, content-irrelevant) file so Probe()'s filesystem existence/size
// checks pass -- ffprobe itself is mocked, so the content never actually gets decoded.
class ScratchFile {
public:
    explicit ScratchFile(const std::string& suffix) {
        path_ = std::filesystem::temp_directory_path() /
                ("ffmpeg_engine_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + suffix);
        std::ofstream out(path_, std::ios::binary);
        out << "not a real media file";
    }
    ~ScratchFile() { std::error_code ec; std::filesystem::remove(path_, ec); }

    std::string string() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

const char* kCannedProbeJson = R"JSON(
{
  "streams": [
    {"codec_type": "video", "codec_name": "h264", "width": 1920, "height": 1080, "r_frame_rate": "30000/1001"},
    {"codec_type": "audio", "codec_name": "aac"}
  ],
  "format": {"duration": "12.345000", "bit_rate": "128000"}
}
)JSON";

}  // namespace

// Successful executable discovery is memoized process-wide (see FFmpegDiscovery.h). These
// tests script a MockProcessRunner to answer the lookup differently each time, so each one
// has to start from an empty cache or it would silently assert against the previous test's
// answer.
class FFmpegEngineTest : public ::testing::Test {
protected:
    void SetUp() override { mediatool::media::ResetDiscoveryCacheForTesting(); }
    void TearDown() override { mediatool::media::ResetDiscoveryCacheForTesting(); }
};

TEST_F(FFmpegEngineTest, IsAvailableFalseWhenDiscoveryFindsNothing) {
    MockProcessRunner runner({}, {}, /*exitCode=*/1);  // "where" finds nothing
    FFmpegEngine engine(runner);
    EXPECT_FALSE(engine.IsAvailable());
}

TEST_F(FFmpegEngineTest, IsAvailableTrueWhenDiscoverySucceeds) {
    MockProcessRunner runner({"C:\\ffmpeg\\bin\\ffmpeg.exe"}, {}, /*exitCode=*/0);
    FFmpegEngine engine(runner);
    EXPECT_TRUE(engine.IsAvailable());
}

TEST_F(FFmpegEngineTest, IsAvailableTrueWithExplicitOverridesAndNoDiscoveryCall) {
    // exitCode=1 would fail discovery, but overrides bypass discovery entirely.
    MockProcessRunner runner({}, {}, /*exitCode=*/1);
    FFmpegEngine engine(runner, std::string("C:\\ffmpeg.exe"), std::string("C:\\ffprobe.exe"));
    EXPECT_TRUE(engine.IsAvailable());
}

TEST_F(FFmpegEngineTest, VersionReturnsFirstStdoutLine) {
    MockProcessRunner runner({"ffmpeg version 6.0-full_build", "built with gcc"}, {}, 0);
    FFmpegEngine engine(runner, std::string("C:\\ffmpeg.exe"));
    auto version = engine.Version();
    ASSERT_TRUE(version.has_value());
    EXPECT_EQ(*version, "ffmpeg version 6.0-full_build");
}

TEST_F(FFmpegEngineTest, VersionNulloptWhenNotDiscoverable) {
    MockProcessRunner runner({}, {}, /*exitCode=*/1);
    FFmpegEngine engine(runner);
    EXPECT_FALSE(engine.Version().has_value());
}

TEST_F(FFmpegEngineTest, ProbeThrowsFileNotFoundForMissingPath) {
    MockProcessRunner runner({}, {}, 0);
    FFmpegEngine engine(runner, std::string("C:\\ffmpeg.exe"), std::string("C:\\ffprobe.exe"));

    try {
        engine.Probe("Z:\\definitely\\does\\not\\exist.mp4");
        FAIL() << "expected MediaToolException";
    } catch (const MediaToolException& ex) {
        EXPECT_EQ(ex.Info().category, ErrorCategory::FileNotFound);
    }
}

TEST_F(FFmpegEngineTest, ProbeThrowsEngineFailureWhenFfprobeNotDiscoverable) {
    ScratchFile file(".mp4");
    MockProcessRunner runner({}, {}, /*exitCode=*/1);  // "where ffprobe" finds nothing
    FFmpegEngine engine(runner);

    try {
        engine.Probe(file.string());
        FAIL() << "expected MediaToolException";
    } catch (const MediaToolException& ex) {
        EXPECT_EQ(ex.Info().category, ErrorCategory::EngineFailure);
    }
}

TEST_F(FFmpegEngineTest, ProbeParsesFfprobeJsonIntoFileInfo) {
    ScratchFile file(".mp4");
    MockProcessRunner runner({kCannedProbeJson}, {}, /*exitCode=*/0);
    FFmpegEngine engine(runner, std::string("C:\\ffmpeg.exe"), std::string("C:\\ffprobe.exe"));

    auto info = engine.Probe(file.string());
    EXPECT_EQ(info.category, mediatool::filesystem::FileCategory::Video);
    EXPECT_EQ(info.extension, "mp4");
    ASSERT_TRUE(info.width.has_value());
    EXPECT_EQ(*info.width, 1920);
    ASSERT_TRUE(info.height.has_value());
    EXPECT_EQ(*info.height, 1080);
    ASSERT_TRUE(info.videoCodec.has_value());
    EXPECT_EQ(*info.videoCodec, "h264");
    ASSERT_TRUE(info.audioCodec.has_value());
    EXPECT_EQ(*info.audioCodec, "aac");
    ASSERT_TRUE(info.durationSeconds.has_value());
    EXPECT_NEAR(*info.durationSeconds, 12.345, 0.001);
    ASSERT_TRUE(info.bitrate.has_value());
    EXPECT_EQ(*info.bitrate, 128000);
    ASSERT_TRUE(info.fps.has_value());
    EXPECT_NEAR(*info.fps, 30000.0 / 1001.0, 0.001);
}

TEST_F(FFmpegEngineTest, ProbeThrowsInvalidFileWhenFfprobeExitsNonZero) {
    ScratchFile file(".mp4");
    MockProcessRunner runner({}, {"moov atom not found"}, /*exitCode=*/1);
    FFmpegEngine engine(runner, std::string("C:\\ffmpeg.exe"), std::string("C:\\ffprobe.exe"));

    try {
        engine.Probe(file.string());
        FAIL() << "expected MediaToolException";
    } catch (const MediaToolException& ex) {
        EXPECT_EQ(ex.Info().category, ErrorCategory::InvalidFile);
    }
}

TEST_F(FFmpegEngineTest, StillUnimplementedOperationsThrowUnsupportedFormat) {
    // Convert/Compress are implemented now (see FFmpegEngineEncodeTest); these two remain
    // declared-but-unimplemented and must say so honestly rather than fake a result.
    MockProcessRunner runner({}, {}, 0);
    FFmpegEngine engine(runner, std::string("C:\\ffmpeg.exe"), std::string("C:\\ffprobe.exe"));
    auto noopProgress = [](const mediatool::jobs::Progress&) {};
    auto neverCancelled = []() { return false; };

    EXPECT_THROW(engine.ExtractAudio("in.mp4", "out.mp3", noopProgress, neverCancelled),
                 MediaToolException);
    EXPECT_THROW(engine.ExtractFrames("in.mp4", "out_dir", {}, noopProgress, neverCancelled),
                 MediaToolException);

    try {
        engine.ExtractAudio("in.mp4", "out.mp3", noopProgress, neverCancelled);
        FAIL() << "expected MediaToolException";
    } catch (const MediaToolException& ex) {
        EXPECT_EQ(ex.Info().category, ErrorCategory::UnsupportedFormat);
    }
}
