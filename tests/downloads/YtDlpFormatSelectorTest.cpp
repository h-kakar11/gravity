#include "engines/downloader/YtDlpFormatSelector.h"

#include <gtest/gtest.h>

using mediatool::downloader::FormatSelectorForQuality;
using mediatool::downloads::QualityPreset;

TEST(YtDlpFormatSelector, BestPrefersSeparateStreamsOverASinglePremuxedFormat) {
    // "bestvideo*" (not "bestvideo") intentionally allows a video-only stream in any
    // container/codec -- spec section 9: best available must not settle for the single
    // highest pre-muxed format if a separate video+audio combination beats it.
    const std::string selector = FormatSelectorForQuality(QualityPreset::Best);
    EXPECT_NE(selector.find("bestvideo*"), std::string::npos);
    EXPECT_NE(selector.find("bestaudio"), std::string::npos);
}

TEST(YtDlpFormatSelector, ResolutionPresetsCapHeight) {
    EXPECT_EQ(FormatSelectorForQuality(QualityPreset::P2160),
              "bestvideo[height<=2160]+bestaudio/best[height<=2160]");
    EXPECT_EQ(FormatSelectorForQuality(QualityPreset::P1440),
              "bestvideo[height<=1440]+bestaudio/best[height<=1440]");
    EXPECT_EQ(FormatSelectorForQuality(QualityPreset::P1080),
              "bestvideo[height<=1080]+bestaudio/best[height<=1080]");
    EXPECT_EQ(FormatSelectorForQuality(QualityPreset::P720),
              "bestvideo[height<=720]+bestaudio/best[height<=720]");
    EXPECT_EQ(FormatSelectorForQuality(QualityPreset::P480),
              "bestvideo[height<=480]+bestaudio/best[height<=480]");
}

TEST(YtDlpFormatSelector, AudioOnlyNeverSelectsVideo) {
    const std::string selector = FormatSelectorForQuality(QualityPreset::AudioOnly);
    EXPECT_EQ(selector.find("video"), std::string::npos);
    EXPECT_NE(selector.find("bestaudio"), std::string::npos);
}

// --- IsSafeFormatSelector -------------------------------------------------------------
// Guards downloads::DownloadOptions::formatId, which is user-supplied (the frontend sends
// back an id the user picked from Inspect()'s list) and reaches yt-dlp's -f verbatim.
// The presets above are ours and are deliberately NOT held to this grammar.

TEST(YtDlpFormatSelector, AcceptsTheFormatIdsInspectActuallyReports) {
    using mediatool::downloader::IsSafeFormatSelector;
    // Shapes taken from real extractor output: plain numeric ids, hyphenated/underscored
    // protocol-qualified ids, dotted ids, and the "video+audio" combo a caller builds by
    // joining two of them.
    EXPECT_TRUE(IsSafeFormatSelector("137"));
    EXPECT_TRUE(IsSafeFormatSelector("bestaudio"));
    EXPECT_TRUE(IsSafeFormatSelector("hls-1080p"));
    EXPECT_TRUE(IsSafeFormatSelector("dash_video-2"));
    EXPECT_TRUE(IsSafeFormatSelector("http-1.0"));
    EXPECT_TRUE(IsSafeFormatSelector("137+140"));
}

TEST(YtDlpFormatSelector, RejectsAnythingThatWouldBeAnExpressionRatherThanAnId) {
    using mediatool::downloader::IsSafeFormatSelector;
    // -f is a small expression language, so an unvalidated value there is code, not a
    // name. "all" downloads every stream on the page; a filter picks something the user
    // never saw; a slash silently falls back to a different stream entirely.
    EXPECT_FALSE(IsSafeFormatSelector("bestvideo[height<=1080]"));
    EXPECT_FALSE(IsSafeFormatSelector("137/140"));
    EXPECT_FALSE(IsSafeFormatSelector("137,140"));
    EXPECT_FALSE(IsSafeFormatSelector("(137)"));
    EXPECT_FALSE(IsSafeFormatSelector("137 140"));
    EXPECT_FALSE(IsSafeFormatSelector("\"137\""));
    EXPECT_FALSE(IsSafeFormatSelector("best*"));
    // Malformed combos, and the empty selector.
    EXPECT_FALSE(IsSafeFormatSelector(""));
    EXPECT_FALSE(IsSafeFormatSelector("+137"));
    EXPECT_FALSE(IsSafeFormatSelector("137+"));
    EXPECT_FALSE(IsSafeFormatSelector("137++140"));
    // Bounded length and arity, so a pathological value can't be smuggled past the
    // character check by being enormous.
    EXPECT_FALSE(IsSafeFormatSelector(std::string(65, 'a')));
    EXPECT_FALSE(IsSafeFormatSelector("1+2+3+4+5+6+7+8+9"));
}

TEST(YtDlpFormatSelector, RejectsTheTwoKeywordsThatChangeHowManyStreamsAreDownloaded) {
    using mediatool::downloader::IsSafeFormatSelector;
    // "all" and "mergeall" are plain alphanumeric words, so the character grammar accepts
    // them -- and they turn "download the one stream I picked" into "download every stream
    // on the page". They are the only keywords rejected by name.
    EXPECT_FALSE(IsSafeFormatSelector("all"));
    EXPECT_FALSE(IsSafeFormatSelector("mergeall"));
    EXPECT_FALSE(IsSafeFormatSelector("all+140"));
    EXPECT_FALSE(IsSafeFormatSelector("137+all"));
    // The other bare keywords are not blocked: each still resolves to one stream of the
    // same video, and a site may legitimately name a format id after one of them.
    EXPECT_TRUE(IsSafeFormatSelector("best"));
    EXPECT_TRUE(IsSafeFormatSelector("bestvideo"));
}
