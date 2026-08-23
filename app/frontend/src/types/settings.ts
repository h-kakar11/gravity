// Mirrors core/settings/Settings.h field-for-field. Keep both in sync.
export interface GeneralSettings {
  defaultOutputDirectory: string;
  launchOnStartup: boolean;
  showNotifications: boolean;
}

export interface DownloadSettings {
  defaultQuality: string;
  downloadDirectory: string;
  filenameTemplate: string;
  concurrentDownloads: number;
  speedUnits: "MBps" | "Mbps";
}

export interface ProcessingSettings {
  hardwareAccelerationEnabled: boolean;
  defaultCompressionQuality: "low" | "medium" | "high";
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
}

export interface Settings {
  general: GeneralSettings;
  downloads: DownloadSettings;
  processing: ProcessingSettings;
  privacy: PrivacySettings;
  advanced: AdvancedSettings;
}
