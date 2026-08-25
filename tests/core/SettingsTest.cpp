#include "core/settings/Settings.h"

#include <gtest/gtest.h>

#include <functional>
#include <vector>

#include "core/errors/MediaToolException.h"

namespace mediatool::settings {
namespace {

using mediatool::errors::MediaToolException;

Settings MakeDistinctSettings() {
    Settings settings;

    settings.general.defaultOutputDirectory = "C:\\Users\\test\\Videos";
    settings.general.launchOnStartup = true;
    settings.general.showNotifications = false;
    settings.general.minimizeToTrayOnClose = false;

    settings.downloads.defaultQuality = "1080p";
    settings.downloads.downloadDirectory = "D:\\Downloads\\MediaTool";
    settings.downloads.filenameTemplate = "%(id)s - %(title)s.%(ext)s";
    settings.downloads.concurrentDownloads = 4;
    settings.downloads.speedUnits = "Mbps";

    settings.processing.hardwareAccelerationEnabled = false;
    settings.processing.defaultCompressionQuality = "high";
    settings.processing.defaultOutputFormat = "mp4";
    settings.processing.concurrentJobs = 3;

    settings.privacy.analyticsEnabled = false;
    settings.privacy.crashReportingEnabled = true;

    settings.advanced.ffmpegPath = "C:\\tools\\ffmpeg.exe";
    settings.advanced.ytDlpPath = "C:\\tools\\yt-dlp.exe";
    settings.advanced.temporaryDirectory = "C:\\temp\\mediatool";
    settings.advanced.logLevel = "DEBUG";

    return settings;
}

TEST(SettingsTest, ToJsonNestsUnderExpectedGroupKeys) {
    const Settings settings = MakeDistinctSettings();
    const nlohmann::json json = settings.ToJson();

    ASSERT_TRUE(json.contains("general"));
    ASSERT_TRUE(json.contains("downloads"));
    ASSERT_TRUE(json.contains("processing"));
    ASSERT_TRUE(json.contains("privacy"));
    ASSERT_TRUE(json.contains("advanced"));
}

TEST(SettingsTest, RoundTripPreservesEveryGeneralField) {
    const Settings original = MakeDistinctSettings();
    const Settings roundTripped = Settings::FromJson(original.ToJson());

    EXPECT_EQ(roundTripped.general.defaultOutputDirectory, original.general.defaultOutputDirectory);
    EXPECT_EQ(roundTripped.general.launchOnStartup, original.general.launchOnStartup);
    EXPECT_EQ(roundTripped.general.showNotifications, original.general.showNotifications);
    EXPECT_EQ(roundTripped.general.minimizeToTrayOnClose, original.general.minimizeToTrayOnClose);
}

TEST(SettingsTest, RoundTripPreservesEveryDownloadField) {
    const Settings original = MakeDistinctSettings();
    const Settings roundTripped = Settings::FromJson(original.ToJson());

    EXPECT_EQ(roundTripped.downloads.defaultQuality, original.downloads.defaultQuality);
    EXPECT_EQ(roundTripped.downloads.downloadDirectory, original.downloads.downloadDirectory);
    EXPECT_EQ(roundTripped.downloads.filenameTemplate, original.downloads.filenameTemplate);
    EXPECT_EQ(roundTripped.downloads.concurrentDownloads, original.downloads.concurrentDownloads);
    EXPECT_EQ(roundTripped.downloads.speedUnits, original.downloads.speedUnits);
}

TEST(SettingsTest, RoundTripPreservesEveryProcessingField) {
    const Settings original = MakeDistinctSettings();
    const Settings roundTripped = Settings::FromJson(original.ToJson());

    EXPECT_EQ(roundTripped.processing.hardwareAccelerationEnabled,
              original.processing.hardwareAccelerationEnabled);
    EXPECT_EQ(roundTripped.processing.defaultCompressionQuality,
              original.processing.defaultCompressionQuality);
    EXPECT_EQ(roundTripped.processing.defaultOutputFormat, original.processing.defaultOutputFormat);
    EXPECT_EQ(roundTripped.processing.concurrentJobs, original.processing.concurrentJobs);
}

TEST(SettingsTest, RoundTripPreservesEveryPrivacyField) {
    const Settings original = MakeDistinctSettings();
    const Settings roundTripped = Settings::FromJson(original.ToJson());

    EXPECT_EQ(roundTripped.privacy.analyticsEnabled, original.privacy.analyticsEnabled);
    EXPECT_EQ(roundTripped.privacy.crashReportingEnabled, original.privacy.crashReportingEnabled);
}

TEST(SettingsTest, RoundTripPreservesEveryAdvancedField) {
    const Settings original = MakeDistinctSettings();
    const Settings roundTripped = Settings::FromJson(original.ToJson());

    EXPECT_EQ(roundTripped.advanced.ffmpegPath, original.advanced.ffmpegPath);
    EXPECT_EQ(roundTripped.advanced.ytDlpPath, original.advanced.ytDlpPath);
    EXPECT_EQ(roundTripped.advanced.temporaryDirectory, original.advanced.temporaryDirectory);
    EXPECT_EQ(roundTripped.advanced.logLevel, original.advanced.logLevel);
}

TEST(SettingsTest, FullRoundTripJsonIsIdentical) {
    const Settings original = MakeDistinctSettings();
    const Settings roundTripped = Settings::FromJson(original.ToJson());

    EXPECT_EQ(roundTripped.ToJson(), original.ToJson());
}

TEST(SettingsTest, DefaultsHaveExpectedSentinelValues) {
    const Settings defaults = Settings::Defaults();

    EXPECT_FALSE(defaults.general.launchOnStartup);
    EXPECT_TRUE(defaults.general.showNotifications);
    EXPECT_TRUE(defaults.general.minimizeToTrayOnClose);
    EXPECT_EQ(defaults.downloads.defaultQuality, "best");
    EXPECT_EQ(defaults.downloads.filenameTemplate, "%(title)s.%(ext)s");
    EXPECT_EQ(defaults.downloads.concurrentDownloads, 1);
    EXPECT_EQ(defaults.downloads.speedUnits, "MBps");
    EXPECT_TRUE(defaults.processing.hardwareAccelerationEnabled);
    EXPECT_EQ(defaults.processing.defaultCompressionQuality, "medium");
    EXPECT_EQ(defaults.processing.concurrentJobs, 1);
    // Not user-configurable to true in Phase 1 -- no telemetry backend exists.
    EXPECT_FALSE(defaults.privacy.analyticsEnabled);
    EXPECT_FALSE(defaults.privacy.crashReportingEnabled);
    EXPECT_EQ(defaults.advanced.logLevel, "INFO");
    // downloadDirectory and defaultOutputDirectory should agree with each other by
    // default; both derive from the same %USERPROFILE%\Downloads\MediaTool convention.
    EXPECT_EQ(defaults.general.defaultOutputDirectory, defaults.downloads.downloadDirectory);
}

TEST(SettingsTest, DefaultsRoundTripThroughJson) {
    const Settings defaults = Settings::Defaults();
    const Settings roundTripped = Settings::FromJson(defaults.ToJson());

    EXPECT_EQ(roundTripped.ToJson(), defaults.ToJson());
}

TEST(SettingsTest, DefaultsPassValidation) {
    EXPECT_NO_THROW(Settings::Defaults().Validate());
}

// Regression test for #5: an updateSettings call (or a hand-edited/stale settings file)
// containing an out-of-range or unrecognized value must be rejected rather than silently
// accepted -- the exact "concurrentJobs: 100000" case the audit's fuzz pass found
// accepted with no validation at all.
TEST(SettingsTest, FromJsonRejectsOutOfRangeAndUnrecognizedValues) {
    struct Case {
        const char* name;
        std::function<void(Settings&)> corrupt;
    };
    const std::vector<Case> cases = {
        {"concurrentJobsTooHigh", [](Settings& s) { s.processing.concurrentJobs = 100000; }},
        {"concurrentJobsZero", [](Settings& s) { s.processing.concurrentJobs = 0; }},
        {"concurrentJobsNegative", [](Settings& s) { s.processing.concurrentJobs = -1; }},
        {"concurrentDownloadsTooHigh", [](Settings& s) { s.downloads.concurrentDownloads = 50; }},
        {"speedUnitsBogus", [](Settings& s) { s.downloads.speedUnits = "lightyears"; }},
        {"compressionQualityBogus",
         [](Settings& s) { s.processing.defaultCompressionQuality = "ultra"; }},
        {"logLevelBogus", [](Settings& s) { s.advanced.logLevel = "VERBOSE"; }},
        {"ffmpegPathRelative", [](Settings& s) { s.advanced.ffmpegPath = "..\\..\\ffmpeg.exe"; }},
        {"downloadDirectoryRelative", [](Settings& s) { s.downloads.downloadDirectory = "Downloads"; }},
    };

    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        Settings settings = Settings::Defaults();
        c.corrupt(settings);
        EXPECT_THROW(Settings::FromJson(settings.ToJson()), MediaToolException) << c.name;
        EXPECT_THROW(settings.Validate(), MediaToolException) << c.name;
    }
}

TEST(SettingsTest, ValidateAcceptsWindowsAbsolutePathsAndUncPaths) {
    Settings settings = Settings::Defaults();
    settings.advanced.ffmpegPath = "C:\\tools\\ffmpeg.exe";
    settings.downloads.downloadDirectory = "\\\\server\\share\\Downloads";
    EXPECT_NO_THROW(settings.Validate());
}

TEST(SettingsTest, ValidateAcceptsEmptyOptionalPathFields) {
    Settings settings = Settings::Defaults();
    settings.advanced.ffmpegPath = "";
    settings.advanced.ytDlpPath = "";
    settings.advanced.temporaryDirectory = "";
    EXPECT_NO_THROW(settings.Validate());
}

}  // namespace
}  // namespace mediatool::settings
