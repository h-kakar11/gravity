#pragma once

// Local, file-backed settings (spec section 23). No database in Phase 1 -- see
// core/settings/ISettingsStore.h for the load/save abstraction. Field groups mirror the
// product spec exactly; keep docs/ipc-contract.md's "Settings" section in sync with this
// struct, and app/frontend/src/types/settings.ts in sync with both.

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace mediatool::settings {

struct GeneralSettings {
    std::string defaultOutputDirectory;
    bool launchOnStartup = false;
    bool showNotifications = true;
};

struct DownloadSettings {
    std::string defaultQuality = "best";
    std::string downloadDirectory;
    std::string filenameTemplate = "%(title)s.%(ext)s";
    int concurrentDownloads = 1;
    std::string speedUnits = "MBps";  // "MBps" | "Mbps"
};

struct ProcessingSettings {
    bool hardwareAccelerationEnabled = true;
    std::string defaultCompressionQuality = "medium";  // "low" | "medium" | "high"
    std::string defaultOutputFormat;
    int concurrentJobs = 1;
};

struct PrivacySettings {
    // Always false. Not user-configurable to "true" -- there is no telemetry backend to
    // enable (spec section 24). Present as a field only so the frontend has something
    // concrete to display ("Analytics: Disabled") rather than nothing.
    bool analyticsEnabled = false;
    bool crashReportingEnabled = false;
};

struct AdvancedSettings {
    std::string ffmpegPath;    // empty = auto-discover, see engines/ffmpeg
    std::string ytDlpPath;     // empty = use bundled python/downloader venv
    std::string temporaryDirectory;  // empty = %LOCALAPPDATA%\MediaTool\temp
    std::string logLevel = "INFO";   // "DEBUG" | "INFO" | "WARNING" | "ERROR"
    // Off by default: HandleCreateDownloadJob/HandleInspectFile reject UNC output
    // directories unless this is explicitly turned on (spec/audit #11).
    bool allowNetworkPaths = false;
};

struct Settings {
    GeneralSettings general;
    DownloadSettings downloads;
    ProcessingSettings processing;
    PrivacySettings privacy;
    AdvancedSettings advanced;

    nlohmann::json ToJson() const;
    // Parses `json` and then calls Validate() on the result -- FromJson never returns an
    // out-of-range or malformed Settings object, it throws instead (see Validate()).
    static Settings FromJson(const nlohmann::json& json);
    static Settings Defaults();

    // Throws errors::MediaToolException{ErrorCategory::Unknown, "E_INVALID_SETTINGS", ...}
    // on the first field that is out of its allowed range/enum, or a non-empty path field
    // that isn't a well-formed absolute path (existence is not required -- an output
    // directory that hasn't been created yet is still valid). Exists so that neither
    // updateSettings (a malicious or fat-fingered IPC call) nor loading a hand-edited or
    // stale settings file from disk can ever put the app into a state that crashes or
    // permanently fails to start (spec/audit #5).
    void Validate() const;
};

}  // namespace mediatool::settings
