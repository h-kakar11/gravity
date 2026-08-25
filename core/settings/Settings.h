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
};

struct Settings {
    GeneralSettings general;
    DownloadSettings downloads;
    ProcessingSettings processing;
    PrivacySettings privacy;
    AdvancedSettings advanced;

    nlohmann::json ToJson() const;
    static Settings FromJson(const nlohmann::json& json);
    static Settings Defaults();
};

}  // namespace mediatool::settings
