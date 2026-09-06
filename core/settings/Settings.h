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
    std::string defaultQuality = "best";
    std::string downloadDirectory;
    std::string filenameTemplate = "%(title)s.%(ext)s";
    int concurrentDownloads = 1;
    // "KBps" | "KiBps" | "MBps" | "MiBps" | "GBps" | "GiBps" | "Kbps" | "Kibps" | "Mbps" | "Mibps" | "Gbps" | "Gibps"
    std::string speedUnits = "MBps";
};

struct ProcessingSettings {
    bool hardwareAccelerationEnabled = true;
    // "lowest" | "low" | "medium" | "high" | "ultra" (issue #59: wanted an explicit
    // smallest/largest option at each end rather than just the three middle presets).
    std::string defaultCompressionQuality = "medium";
    std::string defaultOutputFormat;
    // How many jobs the worker pool runs at once (1-25). Not 1: a batch of conversions
    // submitted together -- which is the whole point of picking several files or a folder
    // on the Convert screen -- would otherwise crawl through strictly one at a time, and
    // "one at a time" is indistinguishable from "batch convert is broken" to the person
    // watching it. 3 is deliberately modest rather than core-count-derived: ffmpeg already
    // threads a single encode across the CPU, so the gain from running more at once comes
    // from filling the gaps (probe, container muxing, disk I/O) around that, and past a
    // handful the encodes mostly just contend. Users with a hardware encoder that likes
    // more sessions can raise it in Settings, which now applies without a restart.
    int concurrentJobs = 3;
    // TOTAL attempts for a job that fails recoverably, including the first -- so 1
    // disables automatic retry and 3 means "the original plus two retries". Lives beside
    // concurrentJobs rather than under `advanced` because, like concurrentJobs, it is a
    // JobManager-wide policy rather than anything specific to media processing. See
    // core/jobs/RetryPolicy.h for which failures are eligible at all.
    int maxRetryAttempts = 3;
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
