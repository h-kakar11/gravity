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
