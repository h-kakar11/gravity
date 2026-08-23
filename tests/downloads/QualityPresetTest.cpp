#include "core/downloads/QualityPreset.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

using mediatool::downloads::QualityPreset;
using mediatool::downloads::QualityPresetFromWireString;
using mediatool::downloads::ToWireString;

TEST(QualityPreset, WireStringsRoundTripForEveryValue) {
    const std::vector<QualityPreset> all = {
        QualityPreset::Best,  QualityPreset::P2160,     QualityPreset::P1440, QualityPreset::P1080,
        QualityPreset::P720,  QualityPreset::P480,      QualityPreset::AudioOnly,
    };
    for (const auto preset : all) {
        EXPECT_EQ(QualityPresetFromWireString(ToWireString(preset)), preset);
    }
}

TEST(QualityPreset, WireStringsMatchDocumentedValues) {
    EXPECT_EQ(ToWireString(QualityPreset::Best), "BEST");
    EXPECT_EQ(ToWireString(QualityPreset::P2160), "2160P");
    EXPECT_EQ(ToWireString(QualityPreset::P1440), "1440P");
    EXPECT_EQ(ToWireString(QualityPreset::P1080), "1080P");
    EXPECT_EQ(ToWireString(QualityPreset::P720), "720P");
    EXPECT_EQ(ToWireString(QualityPreset::P480), "480P");
    EXPECT_EQ(ToWireString(QualityPreset::AudioOnly), "AUDIO_ONLY");
}

TEST(QualityPreset, FromWireStringThrowsOnUnrecognizedValue) {
    EXPECT_THROW(QualityPresetFromWireString("NOT_A_PRESET"), std::invalid_argument);
    EXPECT_THROW(QualityPresetFromWireString(""), std::invalid_argument);
    EXPECT_THROW(QualityPresetFromWireString("best"), std::invalid_argument)
        << "wire strings are UPPER_SNAKE_CASE per docs/ipc-contract.md -- lowercase must not match";
}
