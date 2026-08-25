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

TEST_F(JsonFileSettingsStoreTest, LoadOnCorruptedFileThrowsInvalidFile) {
    JsonFileSettingsStore store(settingsPath_);
    store.Save(Settings::Defaults());
    ASSERT_TRUE(fs::exists(settingsPath_));

    {
        std::ofstream corrupt(settingsPath_, std::ios::binary | std::ios::trunc);
        corrupt << "{ this is not valid json ][";
    }

    bool threw = false;
    try {
        store.Load();
    } catch (const errors::MediaToolException& e) {
        threw = true;
        EXPECT_EQ(e.Info().category, errors::ErrorCategory::InvalidFile);
    }
    EXPECT_TRUE(threw);
}

TEST_F(JsonFileSettingsStoreTest, LoadOnValidJsonMissingSettingsFieldsThrowsInvalidFile) {
    {
        std::ofstream validButWrongShape(settingsPath_, std::ios::binary | std::ios::trunc);
        validButWrongShape << R"({"unrelated": true})";
    }

    JsonFileSettingsStore store(settingsPath_);
    bool threw = false;
    try {
        store.Load();
    } catch (const errors::MediaToolException& e) {
        threw = true;
        EXPECT_EQ(e.Info().category, errors::ErrorCategory::InvalidFile);
    }
    EXPECT_TRUE(threw);
}

}  // namespace
}  // namespace mediatool::settings
