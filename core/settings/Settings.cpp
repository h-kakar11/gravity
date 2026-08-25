#include "core/settings/Settings.h"

#include <cstdlib>
#include <unordered_set>

#include "core/errors/ErrorInfo.h"
#include "core/errors/MediaToolException.h"
#include "core/filesystem/PathUtils.h"

namespace mediatool::settings {

namespace {

[[noreturn]] void ThrowInvalid(const std::string& reason) {
    throw errors::MediaToolException(errors::ErrorInfo::Make(
        "E_INVALID_SETTINGS", errors::ErrorCategory::Unknown,
        "Settings failed validation.", reason));
}

void RequireInRange(int value, int min, int max, const std::string& field) {
    if (value < min || value > max) {
        ThrowInvalid(field + " must be between " + std::to_string(min) + " and " +
                     std::to_string(max) + " (got " + std::to_string(value) + ")");
    }
}

void RequireOneOf(const std::string& value, const std::unordered_set<std::string>& allowed,
                   const std::string& field) {
    if (!allowed.count(value)) {
        ThrowInvalid(field + " has an unrecognized value: '" + value + "'");
    }
}

// A non-empty path field must be a well-formed absolute path; it need not exist yet.
void RequireAbsoluteIfPresent(const std::string& value, const std::string& field) {
    if (value.empty()) return;  // empty means "use the default" for every path field here
    if (!filesystem::paths::LooksAbsoluteWindowsPath(value)) {
        ThrowInvalid(field + " must be an absolute path: '" + value + "'");
    }
}

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
    return userProfile + "\\Downloads\\Gravity";
}

}  // namespace

nlohmann::json Settings::ToJson() const {
    return nlohmann::json{
        {"general",
         {
             {"defaultOutputDirectory", general.defaultOutputDirectory},
             {"launchOnStartup", general.launchOnStartup},
             {"showNotifications", general.showNotifications},
             {"minimizeToTrayOnClose", general.minimizeToTrayOnClose},
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
             {"allowNetworkPaths", advanced.allowNetworkPaths},
         }},
    };
}

Settings Settings::FromJson(const nlohmann::json& json) {
    Settings settings;

    const auto& general = json.at("general");
    settings.general.defaultOutputDirectory = general.at("defaultOutputDirectory").get<std::string>();
    settings.general.launchOnStartup = general.at("launchOnStartup").get<bool>();
    settings.general.showNotifications = general.at("showNotifications").get<bool>();
    settings.general.minimizeToTrayOnClose = general.value("minimizeToTrayOnClose", true);

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
    settings.advanced.allowNetworkPaths = advanced.value("allowNetworkPaths", false);

    settings.Validate();
    return settings;
}

void Settings::Validate() const {
    RequireInRange(downloads.concurrentDownloads, 1, 8, "downloads.concurrentDownloads");
    RequireOneOf(downloads.speedUnits, {"MBps", "Mbps"}, "downloads.speedUnits");

    RequireInRange(processing.concurrentJobs, 1, 16, "processing.concurrentJobs");
    RequireOneOf(processing.defaultCompressionQuality, {"low", "medium", "high"},
                 "processing.defaultCompressionQuality");

    RequireOneOf(advanced.logLevel, {"DEBUG", "INFO", "WARNING", "ERROR"}, "advanced.logLevel");

    RequireAbsoluteIfPresent(general.defaultOutputDirectory, "general.defaultOutputDirectory");
    RequireAbsoluteIfPresent(downloads.downloadDirectory, "downloads.downloadDirectory");
    RequireAbsoluteIfPresent(advanced.ffmpegPath, "advanced.ffmpegPath");
    RequireAbsoluteIfPresent(advanced.ytDlpPath, "advanced.ytDlpPath");
    RequireAbsoluteIfPresent(advanced.temporaryDirectory, "advanced.temporaryDirectory");
}

Settings Settings::Defaults() {
    Settings settings;

    const std::string defaultDir = DefaultUserOutputDirectory();
    settings.general.defaultOutputDirectory = defaultDir;
    settings.general.launchOnStartup = false;
    settings.general.showNotifications = true;
    settings.general.minimizeToTrayOnClose = true;

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
    settings.advanced.allowNetworkPaths = false;

    return settings;
}

}  // namespace mediatool::settings
