#include "core/filesystem/TempDirectory.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace stdfs = std::filesystem;
using mediatool::filesystem::TempDirectory;

namespace {

// Isolated base dir under the system temp dir -- this override is exactly what keeps
// these tests from ever touching the real %LOCALAPPDATA%.
class TempDirectoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        baseDir_ = (stdfs::temp_directory_path() / "mediatool_tempdir_test").string();
        std::error_code ec;
        stdfs::remove_all(baseDir_, ec);
    }

    void TearDown() override {
        std::error_code ec;
        stdfs::remove_all(baseDir_, ec);
    }

    std::string baseDir_;
};

}  // namespace

TEST_F(TempDirectoryTest, CreatesDirectoryUnderOverrideBase) {
    TempDirectory dir("abc123", baseDir_);
    EXPECT_TRUE(stdfs::exists(dir.Path()));
    const stdfs::path expected = stdfs::path(baseDir_) / "Gravity" / "temp" / "job-abc123";
    EXPECT_EQ(stdfs::path(dir.Path()), expected);
}

TEST_F(TempDirectoryTest, DoesNotDoublePrefixAnAlreadyPrefixedJobId) {
    TempDirectory dir("job-already-prefixed", baseDir_);
    const stdfs::path expected =
        stdfs::path(baseDir_) / "Gravity" / "temp" / "job-already-prefixed";
    EXPECT_EQ(stdfs::path(dir.Path()), expected);
}

TEST_F(TempDirectoryTest, DeletesDirectoryOnDestruction) {
    std::string path;
    {
        TempDirectory dir("cleanup-test", baseDir_);
        path = dir.Path();
        ASSERT_TRUE(stdfs::exists(path));
    }
    EXPECT_FALSE(stdfs::exists(path));
}

TEST_F(TempDirectoryTest, ReleasePreventsCleanupOnDestruction) {
    std::string path;
    {
        TempDirectory dir("release-test", baseDir_);
        path = dir.Path();
        EXPECT_FALSE(dir.IsReleased());
        dir.Release();
        EXPECT_TRUE(dir.IsReleased());
    }
    EXPECT_TRUE(stdfs::exists(path));
}

TEST_F(TempDirectoryTest, ReleaseIsIdempotent) {
    TempDirectory dir("idempotent-test", baseDir_);
    dir.Release();
    dir.Release();
    EXPECT_TRUE(dir.IsReleased());
}

TEST_F(TempDirectoryTest, MoveConstructionTransfersOwnership) {
    std::string path;
    TempDirectory outer("move-test", baseDir_);
    path = outer.Path();

    TempDirectory inner(std::move(outer));
    EXPECT_EQ(inner.Path(), path);
    ASSERT_TRUE(stdfs::exists(path));

    // outer is moved-from and must not delete the directory when it goes out of scope;
    // only inner's destructor should.
}

TEST_F(TempDirectoryTest, FilesWrittenInsideSurviveUntilDestruction) {
    std::string filePath;
    {
        TempDirectory dir("with-file-test", baseDir_);
        filePath = (stdfs::path(dir.Path()) / "output.txt").string();
        std::ofstream(filePath) << "hello";
        ASSERT_TRUE(stdfs::exists(filePath));
    }
    EXPECT_FALSE(stdfs::exists(filePath));
}
