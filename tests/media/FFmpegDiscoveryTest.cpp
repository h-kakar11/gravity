// Discovery order (Phase 7, "do not assume FFmpeg is installed separately"): override,
// then a binary bundled next to the running executable, then a PATH lookup. The bundled
// step is exercised for real -- it looks beside *this test binary's own* executable
// directory (ExecutableDirectory() has no test seam; it answers "where am I actually
// running from" for whatever process calls it, and that's exactly what this test wants
// to prove works) -- by planting a real file there and confirming it's found without the
// PATH-lookup runner ever being asked.

#include "engines/ffmpeg/FFmpegDiscovery.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

#include "core/filesystem/ExecutablePath.h"
#include "core/process/MockProcessRunner.h"

using mediatool::process::MockProcessRunner;

namespace {

namespace stdfs = std::filesystem;

#ifdef _WIN32
constexpr const char* kExeSuffix = ".exe";
#else
constexpr const char* kExeSuffix = "";
#endif

class FFmpegDiscoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        mediatool::media::ResetDiscoveryCacheForTesting();
        auto exeDir = mediatool::filesystem::ExecutableDirectory();
        ASSERT_TRUE(exeDir.has_value());
        binDir_ = stdfs::path(*exeDir) / "bin";
        std::error_code ec;
        stdfs::create_directories(binDir_, ec);
    }

    void TearDown() override {
        std::error_code ec;
        stdfs::remove_all(binDir_, ec);
        mediatool::media::ResetDiscoveryCacheForTesting();
    }

    // Creates <exeDir>/bin/<name><suffix> as a real, discoverable (non-empty) file.
    std::string PlantBundled(const std::string& name) {
        auto path = binDir_ / (name + kExeSuffix);
        std::ofstream out(path, std::ios::binary);
        out << "not a real binary, just needs to exist";
        return path.string();
    }

    stdfs::path binDir_;
};

TEST_F(FFmpegDiscoveryTest, PrefersABundledBinaryOverAPathLookup) {
    const std::string planted = PlantBundled("ffmpeg");
    // A runner that would fail the test if actually asked to shell out to `where`/`which`
    // -- exit code 1, no output -- so a pass here proves the bundled path won, not that
    // the PATH lookup happened to agree with it.
    MockProcessRunner runner({}, {}, /*exitCode=*/1);

    auto resolved = mediatool::media::DiscoverFfmpegPath(runner);

    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(*resolved, planted);
}

TEST_F(FFmpegDiscoveryTest, AnExplicitOverrideStillWinsOverABundledBinary) {
    PlantBundled("ffmpeg");
    MockProcessRunner runner({}, {}, /*exitCode=*/1);

    auto resolved = mediatool::media::DiscoverFfmpegPath(runner, std::string("C:/pinned/ffmpeg.exe"));

    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(*resolved, "C:/pinned/ffmpeg.exe");
}

TEST_F(FFmpegDiscoveryTest, FallsBackToPathLookupWhenNothingIsBundled) {
    // bin/ deliberately left empty -- no ffprobe planted.
    MockProcessRunner runner({"/usr/bin/ffprobe"}, {}, /*exitCode=*/0);

    auto resolved = mediatool::media::DiscoverFfprobePath(runner);

    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(*resolved, "/usr/bin/ffprobe");
}

}  // namespace
