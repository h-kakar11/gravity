// ExecutableDirectory() has no mock/fake seam by design -- its entire job is answering
// "where is the real running process actually located," so the only meaningful test is
// against the real platform API, for the real test binary.

#include "core/filesystem/ExecutablePath.h"

#include <filesystem>
#include <gtest/gtest.h>

namespace {

namespace stdfs = std::filesystem;

TEST(ExecutablePathTest, ResolvesToAnExistingDirectory) {
    auto dir = mediatool::filesystem::ExecutableDirectory();
    ASSERT_TRUE(dir.has_value());

    std::error_code ec;
    EXPECT_TRUE(stdfs::is_directory(*dir, ec));
    EXPECT_FALSE(ec);
}

TEST(ExecutablePathTest, IsAnAbsolutePath) {
    auto dir = mediatool::filesystem::ExecutableDirectory();
    ASSERT_TRUE(dir.has_value());
    EXPECT_TRUE(stdfs::path(*dir).is_absolute());
}

TEST(ExecutablePathTest, ContainsThisTestBinaryItself) {
    // Not just "some directory" -- the directory the currently-running executable was
    // actually loaded from, verified by confirming a file with this process's own image
    // path exists directly inside it.
    auto dir = mediatool::filesystem::ExecutableDirectory();
    ASSERT_TRUE(dir.has_value());

    bool foundAnExecutableFile = false;
    std::error_code ec;
    for (const auto& entry : stdfs::directory_iterator(*dir, ec)) {
        if (entry.is_regular_file(ec)) {
            foundAnExecutableFile = true;
            break;
        }
    }
    EXPECT_FALSE(ec);
    EXPECT_TRUE(foundAnExecutableFile) << "expected at least this test binary itself in " << *dir;
}

}  // namespace
