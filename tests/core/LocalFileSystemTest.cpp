#include "core/filesystem/LocalFileSystem.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/errors/MediaToolException.h"

namespace stdfs = std::filesystem;
using mediatool::errors::MediaToolException;
using mediatool::filesystem::FileCategory;
using mediatool::filesystem::LocalFileSystem;

namespace {

class LocalFileSystemTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = (stdfs::temp_directory_path() / "mediatool_lfs_test").string();
        std::error_code ec;
        stdfs::remove_all(root_, ec);
        stdfs::create_directories(root_);
    }

    void TearDown() override {
        std::error_code ec;
        stdfs::remove_all(root_, ec);
    }

    std::string WriteFile(const std::string& relativeName, const std::string& contents) {
        const std::string path = (stdfs::path(root_) / relativeName).string();
        std::ofstream out(path, std::ios::binary);
        out << contents;
        out.close();
        return path;
    }

    std::string root_;
    LocalFileSystem fs_;
};

}  // namespace

TEST_F(LocalFileSystemTest, ExistsTrueForRealFileFalseOtherwise) {
    const std::string path = WriteFile("clip.mp4", "fake-data");
    EXPECT_TRUE(fs_.Exists(path));
    EXPECT_FALSE(fs_.Exists((stdfs::path(root_) / "missing.mp4").string()));
}

TEST_F(LocalFileSystemTest, InspectPopulatesNonMediaFieldsAndLeavesMediaFieldsEmpty) {
    const std::string path = WriteFile("clip.MP4", "0123456789");
    const auto info = fs_.Inspect(path);

    EXPECT_EQ(info.path, path);
    EXPECT_EQ(info.filename, "clip.MP4");
    EXPECT_EQ(info.extension, "mp4");
    EXPECT_EQ(info.category, FileCategory::Video);
    EXPECT_EQ(info.sizeBytes, 10u);
    ASSERT_TRUE(info.mimeType.has_value());
    EXPECT_EQ(*info.mimeType, "video/mp4");

    EXPECT_FALSE(info.durationSeconds.has_value());
    EXPECT_FALSE(info.width.has_value());
    EXPECT_FALSE(info.height.has_value());
    EXPECT_FALSE(info.videoCodec.has_value());
    EXPECT_FALSE(info.audioCodec.has_value());
    EXPECT_FALSE(info.bitrate.has_value());
    EXPECT_FALSE(info.fps.has_value());
}

TEST_F(LocalFileSystemTest, InspectClassifiesUnknownExtensionAsUnknown) {
    const std::string path = WriteFile("data.xyz123", "abc");
    const auto info = fs_.Inspect(path);
    EXPECT_EQ(info.category, FileCategory::Unknown);
    EXPECT_FALSE(info.mimeType.has_value());
}

TEST_F(LocalFileSystemTest, InspectThrowsFileNotFoundForMissingPath) {
    const std::string missing = (stdfs::path(root_) / "missing.mp4").string();
    try {
        fs_.Inspect(missing);
        FAIL() << "expected MediaToolException";
    } catch (const MediaToolException& ex) {
        EXPECT_EQ(ex.Info().category, mediatool::errors::ErrorCategory::FileNotFound);
    }
}

TEST_F(LocalFileSystemTest, GetExtensionFilenameParentDirectory) {
    const std::string path = WriteFile("song.mp3", "x");
    EXPECT_EQ(fs_.GetExtension(path), "mp3");
    EXPECT_EQ(fs_.GetFilename(path), "song.mp3");
    EXPECT_EQ(fs_.GetParentDirectory(path), root_);
}

TEST_F(LocalFileSystemTest, CalculateSizeForSingleFile) {
    const std::string path = WriteFile("blob.bin", "0123456789");
    EXPECT_EQ(fs_.CalculateSize(path), 10u);
}

TEST_F(LocalFileSystemTest, CalculateSizeRecursesIntoDirectories) {
    WriteFile("a.txt", "12345");
    stdfs::create_directories(stdfs::path(root_) / "sub");
    WriteFile("sub/b.txt", "1234567890");
    EXPECT_EQ(fs_.CalculateSize(root_), 15u);
}

TEST_F(LocalFileSystemTest, CopyMoveRenameDelete) {
    const std::string original = WriteFile("original.txt", "hello");
    const std::string copyDest = (stdfs::path(root_) / "copy.txt").string();
    fs_.Copy(original, copyDest);
    EXPECT_TRUE(fs_.Exists(original));
    EXPECT_TRUE(fs_.Exists(copyDest));

    const std::string moveDest = (stdfs::path(root_) / "moved.txt").string();
    fs_.Move(copyDest, moveDest);
    EXPECT_FALSE(fs_.Exists(copyDest));
    EXPECT_TRUE(fs_.Exists(moveDest));

    fs_.Rename(moveDest, "renamed.txt");
    const std::string renamed = (stdfs::path(root_) / "renamed.txt").string();
    EXPECT_FALSE(fs_.Exists(moveDest));
    EXPECT_TRUE(fs_.Exists(renamed));

    fs_.Delete(renamed);
    EXPECT_FALSE(fs_.Exists(renamed));
}

TEST_F(LocalFileSystemTest, CreateDirectoryIsRecursive) {
    const std::string nested = (stdfs::path(root_) / "a" / "b" / "c").string();
    fs_.CreateDirectory(nested);
    EXPECT_TRUE(stdfs::exists(nested));
    EXPECT_TRUE(stdfs::is_directory(nested));
}

TEST_F(LocalFileSystemTest, GetAvailableDiskSpaceReturnsValueForExistingPath) {
    const auto space = fs_.GetAvailableDiskSpace(root_);
    ASSERT_TRUE(space.has_value());
    EXPECT_GT(*space, 0u);
}

TEST_F(LocalFileSystemTest, ListDirectoryReturnsImmediateFilenamesOnly) {
    WriteFile("a.txt", "1");
    WriteFile("b.mp4", "2");
    stdfs::create_directories(stdfs::path(root_) / "sub");
    WriteFile("sub/nested.txt", "3");  // must NOT appear -- non-recursive

    auto names = fs_.ListDirectory(root_);
    std::sort(names.begin(), names.end());
    ASSERT_EQ(names.size(), 3u);  // a.txt, b.mp4, sub
    EXPECT_NE(std::find(names.begin(), names.end(), "a.txt"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "b.mp4"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "sub"), names.end());
}

TEST_F(LocalFileSystemTest, ListDirectoryReturnsEmptyForNonexistentDirectory) {
    const std::string missing = (stdfs::path(root_) / "does_not_exist").string();
    EXPECT_TRUE(fs_.ListDirectory(missing).empty());
}

TEST_F(LocalFileSystemTest, GetAvailableDiskSpaceWalksUpForNonexistentPath) {
    const std::string notYetCreated = (stdfs::path(root_) / "not_created_yet" / "out.mp4").string();
    const auto space = fs_.GetAvailableDiskSpace(notYetCreated);
    ASSERT_TRUE(space.has_value());
    EXPECT_GT(*space, 0u);
}
