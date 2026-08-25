#include "core/settings/JsonFileSettingsStore.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/errors/MediaToolException.h"

namespace mediatool::settings {
namespace {

namespace fs = std::filesystem;

class JsonFileSettingsStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        tempDir_ = fs::temp_directory_path() / "mediatool_json_settings_store_test";
        std::error_code ec;
        fs::remove_all(tempDir_, ec);
        fs::create_directories(tempDir_);
        settingsPath_ = (tempDir_ / "settings.json").string();
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tempDir_, ec);
    }

    std::string ReadFileRaw() const {
        std::ifstream input(settingsPath_, std::ios::binary);
        std::ostringstream stream;
        stream << input.rdbuf();
        return stream.str();
    }

    fs::path tempDir_;
    std::string settingsPath_;
};

TEST_F(JsonFileSettingsStoreTest, LoadOnMissingFileReturnsDefaults) {
    ASSERT_FALSE(fs::exists(settingsPath_));

    JsonFileSettingsStore store(settingsPath_);
    const Settings loaded = store.Load();

    EXPECT_EQ(loaded.ToJson(), Settings::Defaults().ToJson());
    // Load() on a missing file must never create it.
    EXPECT_FALSE(fs::exists(settingsPath_));
}

TEST_F(JsonFileSettingsStoreTest, SaveThenLoadRoundTripsExactly) {
    JsonFileSettingsStore store(settingsPath_);

    Settings original = Settings::Defaults();
    original.general.launchOnStartup = true;
    original.downloads.concurrentDownloads = 5;
    original.downloads.downloadDirectory = "E:\\Media\\Downloads";
    original.processing.defaultOutputFormat = "mkv";
    original.privacy.crashReportingEnabled = true;
    original.advanced.logLevel = "WARNING";
    original.advanced.ffmpegPath = "E:\\tools\\ffmpeg.exe";

    store.Save(original);
    const Settings loaded = store.Load();

    EXPECT_EQ(loaded.ToJson(), original.ToJson());
}

TEST_F(JsonFileSettingsStoreTest, SaveWritesFileWithExpectedStructure) {
    JsonFileSettingsStore store(settingsPath_);
    const Settings settings = Settings::Defaults();
    store.Save(settings);

    ASSERT_TRUE(fs::exists(settingsPath_));

    const std::string raw = ReadFileRaw();
    // Pretty-printed (dump with indent) -- not a single-line blob.
    EXPECT_NE(raw.find('\n'), std::string::npos);

    const nlohmann::json parsed = nlohmann::json::parse(raw);
    ASSERT_TRUE(parsed.contains("general"));
    ASSERT_TRUE(parsed.contains("downloads"));
    ASSERT_TRUE(parsed.contains("processing"));
    ASSERT_TRUE(parsed.contains("privacy"));
    ASSERT_TRUE(parsed.contains("advanced"));
    EXPECT_EQ(parsed["downloads"]["defaultQuality"], "best");

    // No leftover temp file after a successful save.
    EXPECT_FALSE(fs::exists(settingsPath_ + ".tmp"));
}

TEST_F(JsonFileSettingsStoreTest, SaveCreatesParentDirectoriesIfMissing) {
    const fs::path nestedPath = tempDir_ / "nested" / "deeper" / "settings.json";
    JsonFileSettingsStore store(nestedPath.string());

    store.Save(Settings::Defaults());

    EXPECT_TRUE(fs::exists(nestedPath));
}

// Regression test for #5 (the "brick on next launch" half): a corrupt settings file on
// disk must never prevent the app from starting -- Load() falls back to defaults instead
// of throwing, and leaves the corrupt file on disk untouched.
TEST_F(JsonFileSettingsStoreTest, LoadOnCorruptedFileFallsBackToDefaultsWithoutThrowing) {
    JsonFileSettingsStore store(settingsPath_);
    store.Save(Settings::Defaults());
    ASSERT_TRUE(fs::exists(settingsPath_));

    {
        std::ofstream corrupt(settingsPath_, std::ios::binary | std::ios::trunc);
        corrupt << "{ this is not valid json ][";
    }

    const Settings loaded = store.Load();
    EXPECT_EQ(loaded.ToJson(), Settings::Defaults().ToJson());
    // The corrupt file itself is left alone, not silently overwritten.
    EXPECT_EQ(ReadFileRaw(), "{ this is not valid json ][");
}

TEST_F(JsonFileSettingsStoreTest, LoadOnValidJsonMissingSettingsFieldsFallsBackToDefaults) {
    {
        std::ofstream validButWrongShape(settingsPath_, std::ios::binary | std::ios::trunc);
        validButWrongShape << R"({"unrelated": true})";
    }

    JsonFileSettingsStore store(settingsPath_);
    const Settings loaded = store.Load();
    EXPECT_EQ(loaded.ToJson(), Settings::Defaults().ToJson());
}

// Regression test for #5 (the "unvalidated settings" half, at the store layer): a
// settings file containing a well-formed but out-of-range value (as `updateSettings`
// could previously write with no validation at all) must also fall back to defaults on
// the next load, rather than throwing and preventing startup.
TEST_F(JsonFileSettingsStoreTest, LoadOnOutOfRangeValueFallsBackToDefaults) {
    Settings bad = Settings::Defaults();
    bad.processing.concurrentJobs = 100000;
    {
        JsonFileSettingsStore writer(settingsPath_);
        // Bypass Settings::Validate() (which Save() itself doesn't call) to simulate a
        // file written before validation existed, or hand-edited directly.
        std::ofstream out(settingsPath_, std::ios::binary | std::ios::trunc);
        out << bad.ToJson().dump(2);
    }

    JsonFileSettingsStore store(settingsPath_);
    const Settings loaded = store.Load();
    EXPECT_EQ(loaded.ToJson(), Settings::Defaults().ToJson());
}

}  // namespace
}  // namespace mediatool::settings
