#include "core/settings/Settings.h"

#include <cstdlib>
#include <unordered_set>
#include <vector>

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

// Loose validation of an Electron/tauri-plugin-global-shortcut accelerator string, e.g.
// "CommandOrControl+Shift+D": every "+"-separated segment but the last must be a
// recognized modifier name, and the last segment must be non-empty. This is defense in
// depth (a malformed string would otherwise just fail silently to register in Rust), not a
// full grammar check -- Rust's registration call is still the source of truth for whether a
// given string is actually a valid accelerator.
void RequireValidHotkeyIfPresent(const std::string& value, const std::string& field) {
    if (value.empty()) return;  // empty means "no binding"

    static const std::unordered_set<std::string> kModifiers = {
        "CommandOrControl", "CmdOrCtrl", "Control", "Ctrl", "Command", "Cmd",
        "Alt", "Option", "Shift", "Super", "Meta",
    };

    // Manual split (not std::getline, which silently drops a trailing empty token) so
    // "CommandOrControl+Shift+" -- a real key missing after the last '+' -- is caught
    // rather than parsed as a valid two-segment accelerator.
    std::vector<std::string> segments;
    size_t start = 0;
    while (true) {
        const size_t plus = value.find('+', start);
        if (plus == std::string::npos) {
            segments.push_back(value.substr(start));
            break;
        }
        segments.push_back(value.substr(start, plus - start));
        start = plus + 1;
    }

    if (segments.empty() || segments.back().empty()) {
        ThrowInvalid(field + " is not a valid accelerator: '" + value + "'");
    }
    for (size_t i = 0; i + 1 < segments.size(); ++i) {
        if (!kModifiers.count(segments[i])) {
            ThrowInvalid(field + " has an unrecognized modifier '" + segments[i] + "' in '" +
                         value + "'");
        }
    }
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
             {"hotkeyPasteAndDownload", general.hotkeyPasteAndDownload},
             {"hotkeyFocusQueue", general.hotkeyFocusQueue},
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
             {"maxRetryAttempts", processing.maxRetryAttempts},
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
    settings.general.hotkeyPasteAndDownload =
        general.value("hotkeyPasteAndDownload", std::string("CommandOrControl+Shift+D"));
    settings.general.hotkeyFocusQueue =
        general.value("hotkeyFocusQueue", std::string("CommandOrControl+Shift+Q"));

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
    // value(), not at(): a settings file written before this field existed is a normal
    // thing to find on disk, and FromJson throwing on it would send LoadFrom() down its
    // "use defaults" path -- silently discarding every setting the user had chosen. An
    // additive field must default, not fail.
    settings.processing.maxRetryAttempts = processing.value("maxRetryAttempts", 3);

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
    RequireInRange(downloads.concurrentDownloads, 1, 10, "downloads.concurrentDownloads");
    RequireOneOf(downloads.speedUnits, {"KBps", "KiBps", "MBps", "MiBps", "GBps", "GiBps", "Kbps", "Kibps", "Mbps", "Mibps", "Gbps", "Gibps"},
                 "downloads.speedUnits");

    RequireInRange(processing.concurrentJobs, 1, 25, "processing.concurrentJobs");
    // Upper bound because each attempt after the first waits out an exponential backoff:
    // ten attempts is already several minutes of a job that will not succeed.
    RequireInRange(processing.maxRetryAttempts, 1, 10, "processing.maxRetryAttempts");
    RequireOneOf(processing.defaultCompressionQuality, {"lowest", "low", "medium", "high", "ultra"},
                 "processing.defaultCompressionQuality");

    RequireOneOf(advanced.logLevel, {"DEBUG", "INFO", "WARNING", "ERROR"}, "advanced.logLevel");

    RequireValidHotkeyIfPresent(general.hotkeyPasteAndDownload, "general.hotkeyPasteAndDownload");
    RequireValidHotkeyIfPresent(general.hotkeyFocusQueue, "general.hotkeyFocusQueue");

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
    settings.general.hotkeyPasteAndDownload = "CommandOrControl+Shift+D";
    settings.general.hotkeyFocusQueue = "CommandOrControl+Shift+Q";

    settings.downloads.defaultQuality = "best";
    settings.downloads.downloadDirectory = defaultDir;
    settings.downloads.filenameTemplate = "%(title)s.%(ext)s";
    settings.downloads.concurrentDownloads = 1;
    settings.downloads.speedUnits = "MBps";

    settings.processing.hardwareAccelerationEnabled = true;
    settings.processing.defaultCompressionQuality = "medium";
    settings.processing.defaultOutputFormat = "";
    settings.processing.concurrentJobs = 1;
    settings.processing.maxRetryAttempts = 3;

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
