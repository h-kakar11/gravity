// Mirrors core/settings/Settings.h field-for-field. Keep both in sync.
export interface GeneralSettings {
  defaultOutputDirectory: string;
  launchOnStartup: boolean;
  showNotifications: boolean;
  // Added ahead of the Phase 4 system-tray feature that reads it -- when true, closing
  // the window hides it to the tray instead of quitting.
  minimizeToTrayOnClose: boolean;
  // Global hotkey bindings (Phase 4.4), Electron/tauri-plugin-global-shortcut accelerator
  // syntax (e.g. "CommandOrControl+Shift+D"). Empty string means "no binding". Rust's
  // hotkeys.rs reads these through getSettings/refresh_hotkeys -- it never owns them.
  hotkeyPasteAndDownload: string;
  hotkeyFocusQueue: string;
}

export type SpeedUnit = "KBps" | "KiBps" | "MBps" | "MiBps" | "GBps" | "GiBps" | "Mbps";

export interface DownloadSettings {
  defaultQuality: string;
  downloadDirectory: string;
  filenameTemplate: string;
  concurrentDownloads: number;
  speedUnits: SpeedUnit;
}

export interface ProcessingSettings {
  hardwareAccelerationEnabled: boolean;
  defaultCompressionQuality: "lowest" | "low" | "medium" | "high" | "ultra";
  defaultOutputFormat: string;
  concurrentJobs: number;
}

export interface PrivacySettings {
  // Always false. There is no telemetry backend to enable (spec section 24) -- shown in
  // the UI as a reassurance, never exposed as an editable toggle.
  analyticsEnabled: false;
  crashReportingEnabled: boolean;
}

export interface AdvancedSettings {
  ffmpegPath: string;
  ytDlpPath: string;
  temporaryDirectory: string;
  logLevel: "DEBUG" | "INFO" | "WARNING" | "ERROR";
  // Off by default -- output/input paths reject UNC (\\server\share) locations unless
  // this is explicitly turned on (spec/audit #11).
  allowNetworkPaths: boolean;
}

export interface Settings {
  general: GeneralSettings;
  downloads: DownloadSettings;
  processing: ProcessingSettings;
  privacy: PrivacySettings;
  advanced: AdvancedSettings;
}
