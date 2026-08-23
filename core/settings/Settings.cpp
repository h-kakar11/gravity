#include "core/settings/Settings.h"

#include <cstdlib>

namespace mediatool::settings {

namespace {

// Reads an environment variable, returning an empty string (never throwing) if unset --
// callers here treat a missing env var as "no reasonable default", not an error.
std::string GetEnvOrEmpty(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : "";
}

std::string DefaultUserOutputDirectory() {
    const std::string userProfile = GetEnvOrEmpty("USERPROFILE");
    if (userProfile.empty()) {
        return "";
    }
    return userProfile + "\\Downloads\\MediaTool";
}

}  // namespace

nlohmann::json Settings::ToJson() const {
    return nlohmann::json{
        {"general",
         {
             {"defaultOutputDirectory", general.defaultOutputDirectory},
             {"launchOnStartup", general.launchOnStartup},
             {"showNotifications", general.showNotifications},
         }},
        {"downloads",
         {
             {"defaultQuality", downloads.defaultQuality},
             {"downloadDirectory", downloads.downloadDirectory},
             {"filenameTemplate", downloads.filenameTemplate},
             {"concurrentDownloads", downloads.concurrentDownloads},
             {"speedUnits", downloads.speedUnits},
         }},
        {"processing",
         {
             {"hardwareAccelerationEnabled", processing.hardwareAccelerationEnabled},
             {"defaultCompressionQuality", processing.defaultCompressionQuality},
             {"defaultOutputFormat", processing.defaultOutputFormat},
             {"concurrentJobs", processing.concurrentJobs},
         }},
        {"privacy",
         {
             {"analyticsEnabled", privacy.analyticsEnabled},
             {"crashReportingEnabled", privacy.crashReportingEnabled},
         }},
        {"advanced",
         {
             {"ffmpegPath", advanced.ffmpegPath},
             {"ytDlpPath", advanced.ytDlpPath},
             {"temporaryDirectory", advanced.temporaryDirectory},
             {"logLevel", advanced.logLevel},
         }},
    };
}

Settings Settings::FromJson(const nlohmann::json& json) {
    Settings settings;

    const auto& general = json.at("general");
    settings.general.defaultOutputDirectory = general.at("defaultOutputDirectory").get<std::string>();
    settings.general.launchOnStartup = general.at("launchOnStartup").get<bool>();
    settings.general.showNotifications = general.at("showNotifications").get<bool>();

    const auto& downloads = json.at("downloads");
    settings.downloads.defaultQuality = downloads.at("defaultQuality").get<std::string>();
    settings.downloads.downloadDirectory = downloads.at("downloadDirectory").get<std::string>();
    settings.downloads.filenameTemplate = downloads.at("filenameTemplate").get<std::string>();
    settings.downloads.concurrentDownloads = downloads.at("concurrentDownloads").get<int>();
    settings.downloads.speedUnits = downloads.at("speedUnits").get<std::string>();

    const auto& processing = json.at("processing");
    settings.processing.hardwareAccelerationEnabled =
        processing.at("hardwareAccelerationEnabled").get<bool>();
    settings.processing.defaultCompressionQuality =
        processing.at("defaultCompressionQuality").get<std::string>();
    settings.processing.defaultOutputFormat = processing.at("defaultOutputFormat").get<std::string>();
    settings.processing.concurrentJobs = processing.at("concurrentJobs").get<int>();

    const auto& privacy = json.at("privacy");
    settings.privacy.analyticsEnabled = privacy.at("analyticsEnabled").get<bool>();
    settings.privacy.crashReportingEnabled = privacy.at("crashReportingEnabled").get<bool>();

    const auto& advanced = json.at("advanced");
    settings.advanced.ffmpegPath = advanced.at("ffmpegPath").get<std::string>();
    settings.advanced.ytDlpPath = advanced.at("ytDlpPath").get<std::string>();
    settings.advanced.temporaryDirectory = advanced.at("temporaryDirectory").get<std::string>();
    settings.advanced.logLevel = advanced.at("logLevel").get<std::string>();

    return settings;
}

Settings Settings::Defaults() {
    Settings settings;

    const std::string defaultDir = DefaultUserOutputDirectory();
    settings.general.defaultOutputDirectory = defaultDir;
    settings.general.launchOnStartup = false;
    settings.general.showNotifications = true;

    settings.downloads.defaultQuality = "best";
    settings.downloads.downloadDirectory = defaultDir;
    settings.downloads.filenameTemplate = "%(title)s.%(ext)s";
    settings.downloads.concurrentDownloads = 1;
    settings.downloads.speedUnits = "MBps";

    settings.processing.hardwareAccelerationEnabled = true;
    settings.processing.defaultCompressionQuality = "medium";
    settings.processing.defaultOutputFormat = "";
    settings.processing.concurrentJobs = 1;

    settings.privacy.analyticsEnabled = false;
    settings.privacy.crashReportingEnabled = false;

    settings.advanced.ffmpegPath = "";
    settings.advanced.ytDlpPath = "";
    settings.advanced.temporaryDirectory = "";
    settings.advanced.logLevel = "INFO";

    return settings;
}

}  // namespace mediatool::settings
