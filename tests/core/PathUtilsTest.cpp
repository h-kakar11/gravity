#include "core/filesystem/PathUtils.h"

#include <gtest/gtest.h>

#include <string>

namespace paths = mediatool::filesystem::paths;

TEST(PathUtilsTest, JoinInsertsSeparator) {
    const std::string joined = paths::Join("C:\\base", "sub\\file.txt");
    EXPECT_EQ(joined, "C:\\base\\sub\\file.txt");
}

TEST(PathUtilsTest, JoinWithAbsoluteComponentReplacesBase) {
    // Matches std::filesystem::path::operator/ semantics: an absolute right-hand side
    // wins outright rather than being appended.
    const std::string joined = paths::Join("C:\\base", "D:\\other\\file.txt");
    EXPECT_EQ(joined, "D:\\other\\file.txt");
}

TEST(PathUtilsTest, JoinVariadicFoldsLeftToRight) {
    const std::string joined = paths::Join("C:\\base", {"a", "b", "c.txt"});
    EXPECT_EQ(joined, "C:\\base\\a\\b\\c.txt");
}

TEST(PathUtilsTest, IsAbsoluteRecognizesWindowsDriveRoot) {
    EXPECT_TRUE(paths::IsAbsolute("C:\\Users\\test"));
    EXPECT_TRUE(paths::IsAbsolute("D:/Videos/file.mp4"));
}

TEST(PathUtilsTest, IsAbsoluteRejectsRelativePaths) {
    EXPECT_FALSE(paths::IsAbsolute("relative\\path"));
    EXPECT_FALSE(paths::IsAbsolute("relative/path"));
    EXPECT_FALSE(paths::IsAbsolute("file.txt"));
}

TEST(PathUtilsTest, NormalizeCollapsesDotAndDotDotSegments) {
    EXPECT_EQ(paths::Normalize("C:\\a\\.\\b\\..\\c"), "C:\\a\\c");
}

TEST(PathUtilsTest, NormalizeConvertsForwardSlashesToPreferredSeparator) {
    EXPECT_EQ(paths::Normalize("C:/a/b/c.txt"), "C:\\a\\b\\c.txt");
}

TEST(PathUtilsTest, GetExtensionStripsDotAndLowercases) {
    EXPECT_EQ(paths::GetExtension("C:\\videos\\My Video.MP4"), "mp4");
}

TEST(PathUtilsTest, GetExtensionEmptyWhenNoExtension) {
    EXPECT_EQ(paths::GetExtension("C:\\videos\\README"), "");
}

TEST(PathUtilsTest, GetFilenameStripsDirectory) {
    EXPECT_EQ(paths::GetFilename("C:\\videos\\My Video.mp4"), "My Video.mp4");
}

TEST(PathUtilsTest, GetParentDirectoryStripsFilename) {
    EXPECT_EQ(paths::GetParentDirectory("C:\\videos\\My Video.mp4"), "C:\\videos");
}

TEST(PathUtilsTest, LooksAbsoluteWindowsPathRecognizesDriveAndUncForms) {
    EXPECT_TRUE(paths::LooksAbsoluteWindowsPath("C:\\Users\\test"));
    EXPECT_TRUE(paths::LooksAbsoluteWindowsPath("D:/Videos/file.mp4"));
    EXPECT_TRUE(paths::LooksAbsoluteWindowsPath("\\\\server\\share\\Downloads"));
    EXPECT_TRUE(paths::LooksAbsoluteWindowsPath("//server/share/Downloads"));
    EXPECT_FALSE(paths::LooksAbsoluteWindowsPath("relative\\path"));
    EXPECT_FALSE(paths::LooksAbsoluteWindowsPath("C:"));
    EXPECT_FALSE(paths::LooksAbsoluteWindowsPath(""));
}

TEST(PathUtilsTest, IsUncPathRecognizesBothSlashDirections) {
    EXPECT_TRUE(paths::IsUncPath("\\\\server\\share"));
    EXPECT_TRUE(paths::IsUncPath("//server/share"));
    EXPECT_FALSE(paths::IsUncPath("C:\\Users\\test"));
}

// Regression tests for #11: output directories and inspected paths were previously
// accepted completely unvalidated -- traversal segments and UNC paths included.
TEST(IsSafeUserSuppliedPathTest, AcceptsPlainAbsoluteWindowsPaths) {
    EXPECT_TRUE(paths::IsSafeUserSuppliedPath("C:\\Users\\test\\Downloads", false));
    EXPECT_TRUE(paths::IsSafeUserSuppliedPath("D:/Videos", false));
}

TEST(IsSafeUserSuppliedPathTest, RejectsTraversalSegments) {
    EXPECT_FALSE(paths::IsSafeUserSuppliedPath("C:\\Users\\..\\Windows\\System32", false));
    EXPECT_FALSE(paths::IsSafeUserSuppliedPath("C:/Users/../../etc", false));
}

TEST(IsSafeUserSuppliedPathTest, RejectsUncPathsByDefault) {
    EXPECT_FALSE(paths::IsSafeUserSuppliedPath("\\\\server\\share\\Downloads", false));
}

TEST(IsSafeUserSuppliedPathTest, AcceptsUncPathsWhenExplicitlyAllowed) {
    EXPECT_TRUE(paths::IsSafeUserSuppliedPath("\\\\server\\share\\Downloads", true));
}

TEST(IsSafeUserSuppliedPathTest, RejectsRelativeAndEmptyPaths) {
    EXPECT_FALSE(paths::IsSafeUserSuppliedPath("relative\\path", false));
    EXPECT_FALSE(paths::IsSafeUserSuppliedPath("", false));
}
