#include "core/filesystem/FilenameSanitizer.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/filesystem/LocalFileSystem.h"

namespace stdfs = std::filesystem;
using mediatool::filesystem::DeduplicateBaseName;
using mediatool::filesystem::DeduplicateFilename;
using mediatool::filesystem::IsJobArtifactOf;
using mediatool::filesystem::LocalFileSystem;
using mediatool::filesystem::SanitizeWindowsFilename;
using mediatool::filesystem::TruncateBaseNameForMaxPath;
using mediatool::filesystem::WithPlaylistIndex;

TEST(FilenameSanitizerTest, ReplacesIllegalWindowsCharacters) {
    const std::string raw = "video: <title> / \"quoted\" | test? * \\ end";
    const std::string result = SanitizeWindowsFilename(raw);

    for (char illegal : std::string("<>:\"/\\|?*")) {
        EXPECT_EQ(result.find(illegal), std::string::npos)
            << "illegal char '" << illegal << "' survived sanitization in: " << result;
    }
}

TEST(FilenameSanitizerTest, StripsControlCharacters) {
    const std::string raw = std::string("before") + '\x01' + '\x1f' + "after";
    EXPECT_EQ(SanitizeWindowsFilename(raw), "beforeafter");
}

TEST(FilenameSanitizerTest, TrimsTrailingDotsAndSpaces) {
    EXPECT_EQ(SanitizeWindowsFilename("My Video..."), "My Video");
    EXPECT_EQ(SanitizeWindowsFilename("My Video   "), "My Video");
    EXPECT_EQ(SanitizeWindowsFilename("My Video. . "), "My Video");
}

TEST(FilenameSanitizerTest, DoesNotTrimLeadingOrInteriorDotsAndSpaces) {
    EXPECT_EQ(SanitizeWindowsFilename(" .My.Video. "), " .My.Video");
}

TEST(FilenameSanitizerTest, TruncatesVeryLongTitlesTo200Characters) {
    const std::string raw(500, 'a');
    const std::string result = SanitizeWindowsFilename(raw);
    EXPECT_EQ(result.size(), 200u);
    EXPECT_EQ(result, std::string(200, 'a'));
}

TEST(FilenameSanitizerTest, TruncationDoesNotSplitMultiByteUtf8) {
    // 300 copies of a 2-byte UTF-8 character (U+00E9, "é") -- a naive byte-length cap
    // at 200 bytes would land mid-codepoint (100 bytes = 50 chars, so 200 bytes lands
    // exactly on a boundary; use an offset that would NOT land cleanly on a byte cap).
    std::string raw;
    for (int i = 0; i < 300; ++i) raw += "\xC3\xA9";  // "é" repeated
    const std::string result = SanitizeWindowsFilename(raw);

    // 200 codepoints * 2 bytes = 400 bytes, and every byte pair must be a complete,
    // valid "é" -- if truncation split a codepoint, this modulo check would fail.
    EXPECT_EQ(result.size(), 400u);
    EXPECT_EQ(result.size() % 2, 0u);
    for (std::size_t i = 0; i < result.size(); i += 2) {
        EXPECT_EQ(static_cast<unsigned char>(result[i]), 0xC3);
        EXPECT_EQ(static_cast<unsigned char>(result[i + 1]), 0xA9);
    }
}

TEST(FilenameSanitizerTest, PreservesUnicodeAndEmojiUntouched) {
    const std::string title = "\xF0\x9F\x8E\xAC" " My Video Title " "\xF0\x9F\x98\x80";
    EXPECT_EQ(SanitizeWindowsFilename(title), title);
}

TEST(FilenameSanitizerTest, FallsBackToUntitledWhenNothingSurvives) {
    EXPECT_EQ(SanitizeWindowsFilename(""), "untitled");
    EXPECT_EQ(SanitizeWindowsFilename("..."), "untitled");
    // Illegal punctuation is replaced with '_' (not removed), so it does not by itself
    // trigger the empty-result fallback -- only control chars/dots/spaces reduce to "".
    EXPECT_EQ(SanitizeWindowsFilename("///"), "___");
    EXPECT_EQ(SanitizeWindowsFilename(std::string("\x01\x02\x03")), "untitled");
}

// Regression tests for #13: Windows reserved device names must be renamed, not passed
// through -- a job that ends up trying to create a file literally named "NUL" or "COM1"
// fails or behaves bizarrely on real Windows filesystems.
TEST(FilenameSanitizerTest, RenamesReservedWindowsDeviceNames) {
    EXPECT_EQ(SanitizeWindowsFilename("CON"), "CON_file");
    EXPECT_EQ(SanitizeWindowsFilename("NUL"), "NUL_file");
    EXPECT_EQ(SanitizeWindowsFilename("con"), "con_file");
    EXPECT_EQ(SanitizeWindowsFilename("COM1"), "COM1_file");
    EXPECT_EQ(SanitizeWindowsFilename("LPT9"), "LPT9_file");
}

TEST(FilenameSanitizerTest, ReservedNameCheckIgnoresExtension) {
    // "NUL.txt" is just as reserved as bare "NUL" -- Windows matches on the stem.
    EXPECT_EQ(SanitizeWindowsFilename("NUL.txt"), "NUL_file.txt");
}

TEST(FilenameSanitizerTest, DoesNotFlagNamesThatMerelyContainAReservedWord) {
    EXPECT_EQ(SanitizeWindowsFilename("CONcert"), "CONcert");
    EXPECT_EQ(SanitizeWindowsFilename("My CON Video"), "My CON Video");
}

TEST(TruncateBaseNameForMaxPathTest, LeavesShortNamesUnchanged) {
    EXPECT_EQ(TruncateBaseNameForMaxPath("C:\\out", "My Video"), "My Video");
}

TEST(TruncateBaseNameForMaxPathTest, TruncatesNamesThatWouldExceedMaxPath) {
    const std::string longTitle(500, 'a');
    const std::string result = TruncateBaseNameForMaxPath("C:\\Users\\test\\Downloads", longTitle);
    EXPECT_LT(result.size(), longTitle.size());
    // "C:\Users\test\Downloads" + "\" + result must leave room for a dedup suffix and an
    // extension, i.e. land comfortably under the legacy 259-character budget.
    EXPECT_LT(std::string("C:\\Users\\test\\Downloads").size() + 1 + result.size(), 259u - 15);
}

TEST(TruncateBaseNameForMaxPathTest, DoesNotSplitMultiByteUtf8) {
    std::string longTitle;
    for (int i = 0; i < 300; ++i) longTitle += "\xC3\xA9";  // "é" repeated (2 bytes each)
    const std::string result = TruncateBaseNameForMaxPath("C:\\out", longTitle);

    EXPECT_EQ(result.size() % 2, 0u);
    for (std::size_t i = 0; i < result.size(); i += 2) {
        EXPECT_EQ(static_cast<unsigned char>(result[i]), 0xC3);
        EXPECT_EQ(static_cast<unsigned char>(result[i + 1]), 0xA9);
    }
}

TEST(TruncateBaseNameForMaxPathTest, LeavesNameUnchangedWhenDirectoryAloneIsAlreadyOverBudget) {
    const std::string hugeDirectory(300, 'd');
    EXPECT_EQ(TruncateBaseNameForMaxPath(hugeDirectory, "My Video"), "My Video");
}

namespace {

class FilenameDedupTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = (stdfs::temp_directory_path() / "mediatool_dedup_test").string();
        std::error_code ec;
        stdfs::remove_all(dir_, ec);
        stdfs::create_directories(dir_);
    }

    void TearDown() override {
        std::error_code ec;
        stdfs::remove_all(dir_, ec);
    }

    void Touch(const std::string& filename) {
        std::ofstream((stdfs::path(dir_) / filename).string()).put('x');
    }

    std::string dir_;
    LocalFileSystem fs_;
};

}  // namespace

TEST_F(FilenameDedupTest, ReturnsDesiredPathWhenFree) {
    const std::string desired = (stdfs::path(dir_) / "video.mp4").string();
    EXPECT_EQ(DeduplicateFilename(desired, fs_), desired);
}

TEST_F(FilenameDedupTest, NumbersSequentiallyOnCollision) {
    Touch("video.mp4");
    const std::string desired = (stdfs::path(dir_) / "video.mp4").string();
    const std::string expected1 = (stdfs::path(dir_) / "video (1).mp4").string();
    EXPECT_EQ(DeduplicateFilename(desired, fs_), expected1);

    Touch("video (1).mp4");
    const std::string expected2 = (stdfs::path(dir_) / "video (2).mp4").string();
    EXPECT_EQ(DeduplicateFilename(desired, fs_), expected2);

    Touch("video (2).mp4");
    const std::string expected3 = (stdfs::path(dir_) / "video (3).mp4").string();
    EXPECT_EQ(DeduplicateFilename(desired, fs_), expected3);
}

TEST_F(FilenameDedupTest, DeduplicateBaseNameReturnsDesiredNameWhenFree) {
    EXPECT_EQ(DeduplicateBaseName(dir_, "video", fs_), "video");
}

TEST_F(FilenameDedupTest, DeduplicateBaseNameNumbersSequentiallyRegardlessOfExtension) {
    // Unlike DeduplicateFilename, the caller doesn't know the final extension yet (spec
    // section 29 -- e.g. yt-dlp only decides the merge container after downloading), so
    // collisions must be detected against ANY extension sharing the base name.
    Touch("video.mp4");
    EXPECT_EQ(DeduplicateBaseName(dir_, "video", fs_), "video (1)");

    Touch("video (1).mkv");
    EXPECT_EQ(DeduplicateBaseName(dir_, "video", fs_), "video (2)");
}

TEST_F(FilenameDedupTest, DeduplicateBaseNameIgnoresUnrelatedFilesSharingAPrefix) {
    Touch("video (backup).mp4");  // stem is "video (backup)", not "video" -- must not collide
    EXPECT_EQ(DeduplicateBaseName(dir_, "video", fs_), "video");
}

TEST(IsJobArtifactOfTest, MatchesExactBaseName) {
    EXPECT_TRUE(IsJobArtifactOf("Clip", "Clip"));
}

TEST(IsJobArtifactOfTest, MatchesOwnOutputAndIntermediateArtifacts) {
    EXPECT_TRUE(IsJobArtifactOf("Clip", "Clip.mp4"));
    EXPECT_TRUE(IsJobArtifactOf("Clip", "Clip.mp4.part"));
    EXPECT_TRUE(IsJobArtifactOf("Clip", "Clip.mp4.ytdl"));
    EXPECT_TRUE(IsJobArtifactOf("Clip", "Clip.f137.mp4"));
    EXPECT_TRUE(IsJobArtifactOf("Clip", "Clip.temp.mp4"));
    EXPECT_TRUE(IsJobArtifactOf("Clip", "Clip.info.json"));
}

TEST(IsJobArtifactOfTest, RejectsUnrelatedFilesSharingOnlyATextPrefix) {
    // The exact reproduction scenario from the audit: a pre-existing file/directory
    // whose name merely starts with the job's title text must never match.
    EXPECT_FALSE(IsJobArtifactOf("Vacation", "Vacation Photos.zip"));
    EXPECT_FALSE(IsJobArtifactOf("Clip", "Clip Backup"));
    EXPECT_FALSE(IsJobArtifactOf("Clip", "Clip Notes.txt"));
    EXPECT_FALSE(IsJobArtifactOf("Clip", "ClipX.mp4"));
}

TEST(IsJobArtifactOfTest, RejectsShorterOrUnrelatedNames) {
    EXPECT_FALSE(IsJobArtifactOf("Clip", "Cli"));
    EXPECT_FALSE(IsJobArtifactOf("Clip", "Other.mp4"));
}

TEST(WithPlaylistIndexTest, ZeroPadsToTotalCountDigitWidth) {
    EXPECT_EQ(WithPlaylistIndex("video.mp4", 3, 42), "03 - video.mp4");
    EXPECT_EQ(WithPlaylistIndex("video.mp4", 5, 100), "005 - video.mp4");
    EXPECT_EQ(WithPlaylistIndex("video.mp4", 1, 5), "1 - video.mp4");
    EXPECT_EQ(WithPlaylistIndex("video.mp4", 42, 42), "42 - video.mp4");
}
