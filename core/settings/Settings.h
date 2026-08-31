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
    // Added ahead of the Phase 4 system-tray implementation that actually reads it, to
    // avoid a second settings-schema migration -- when true, closing the main window
    // hides it to the tray instead of quitting; the Settings toggle lets a user opt back
    // into a normal full quit.
    bool minimizeToTrayOnClose = true;

    // Global hotkey bindings (Phase 4.4), Electron/tauri-plugin-global-shortcut accelerator
    // syntax (e.g. "CommandOrControl+Shift+D"). Live in this C++-validated struct rather
    // than a fourth ad hoc Rust JSON file so they get the same range/enum-style checking as
    // every other setting; the Rust side (src-tauri/src/hotkeys.rs) only ever reads them
    // through getSettings, never owns them. Empty means "no binding" -- a user can clear a
    // hotkey without picking a replacement.
    std::string hotkeyPasteAndDownload = "CommandOrControl+Shift+D";
    std::string hotkeyFocusQueue = "CommandOrControl+Shift+Q";
};

struct DownloadSettings {
    // "BEST" | "2160P" | "1440P" | "1080P" | "720P" | "480P" | "AUDIO_ONLY" -- the same
    // wire vocabulary QualityPreset uses everywhere else (createJob's own `quality` field,
    // docs/ipc-contract.md), not an independent casing. Used as HandleCreateDownloadJob's
    // fallback when a createJob request omits `quality` entirely (issue #59/dead-settings
    // audit: this field was stored and round-tripped but never actually consulted).
    std::string defaultQuality = "BEST";
    std::string downloadDirectory;
    std::string filenameTemplate = "%(title)s.%(ext)s";
    int concurrentDownloads = 1;
    // "KBps" | "KiBps" | "MBps" | "MiBps" | "GBps" | "GiBps" | "Mbps"
    std::string speedUnits = "MBps";
};

struct ProcessingSettings {
    // Master kill switch: HandleCreateMediaProcessingJob forces a job's per-request
    // hardwareAcceleration option down to "none" when this is false, regardless of what
    // the request asked for. A per-job request can still choose weaker acceleration or
    // none at all when this is true -- this only ever narrows, never widens, a job's own
    // choice.
    bool hardwareAccelerationEnabled = true;
    // "lowest" | "low" | "medium" | "high" | "ultra" (issue #59: wanted an explicit
    // smallest/largest option at each end rather than just the three middle presets).
    std::string defaultCompressionQuality = "medium";
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
    std::string temporaryDirectory;  // empty = %LOCALAPPDATA%\Gravity\temp
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
