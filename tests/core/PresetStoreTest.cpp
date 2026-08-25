#include "core/settings/PresetStore.h"

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

namespace mediatool::settings {
namespace {

namespace fs = std::filesystem;

class PresetStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        tempDir_ = fs::temp_directory_path() / "mediatool_preset_store_test";
        std::error_code ec;
        fs::remove_all(tempDir_, ec);
        fs::create_directories(tempDir_);
        presetsPath_ = (tempDir_ / "presets.json").string();
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tempDir_, ec);
    }

    fs::path tempDir_;
    std::string presetsPath_;
};

TEST_F(PresetStoreTest, LoadOnMissingFileReturnsEmpty) {
    PresetStore store(presetsPath_);
    EXPECT_TRUE(store.Load().empty());
}

TEST_F(PresetStoreTest, SaveThenLoadRoundTrips) {
    PresetStore store(presetsPath_);
    Preset gaming;
    gaming.id = "preset-1";
    gaming.name = "Gaming";
    gaming.kind = "COMPRESSION";
    gaming.options = {{"outputFormat", "mp4"}, {"quality", "high"}};

    store.Save({gaming});
    const auto loaded = store.Load();

    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].id, "preset-1");
    EXPECT_EQ(loaded[0].name, "Gaming");
    EXPECT_EQ(loaded[0].kind, "COMPRESSION");
    EXPECT_EQ(loaded[0].options.at("outputFormat"), "mp4");
}

TEST_F(PresetStoreTest, SaveOverwritesThePreviousList) {
    PresetStore store(presetsPath_);
    Preset a;
    a.id = "1";
    a.name = "A";
    a.kind = "CONVERSION";
    store.Save({a});

    Preset b;
    b.id = "2";
    b.name = "B";
    b.kind = "DOWNLOAD";
    store.Save({b});

    const auto loaded = store.Load();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].id, "2");
}

TEST_F(PresetStoreTest, LoadOnCorruptFileReturnsEmptyWithoutThrowing) {
    {
        std::ofstream corrupt(presetsPath_, std::ios::binary | std::ios::trunc);
        corrupt << "not json at all [[";
    }
    PresetStore store(presetsPath_);
    EXPECT_NO_THROW({
        const auto loaded = store.Load();
        EXPECT_TRUE(loaded.empty());
    });
}

TEST_F(PresetStoreTest, SkipsUnreadableEntriesWithoutDiscardingTheRest) {
    {
        std::ofstream out(presetsPath_, std::ios::binary | std::ios::trunc);
        out << R"([{"id":"1","name":"Good","kind":"CONVERSION","options":{}},{"missingRequiredFields":true}])";
    }
    PresetStore store(presetsPath_);
    const auto loaded = store.Load();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].id, "1");
}

}  // namespace
}  // namespace mediatool::settings
