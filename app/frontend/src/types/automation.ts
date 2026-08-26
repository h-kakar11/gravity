// Mirrors app/desktop/src-tauri/src/watch_folders.rs and scheduler.rs -- both live
// entirely in Rust (OS-integration work, not job orchestration), so these types back
// Tauri commands invoked directly, never through coreClient.sendCommand's core-process path.

export type AutomationJobType = "CONVERSION" | "COMPRESSION";

export interface WatchFolderConfig {
  path: string;
  jobType: AutomationJobType;
  defaultOptions: Record<string, unknown>;
}

// Unlike Watch Folders, a Scheduled Task isn't reacting to a local file, so DOWNLOAD is a
// meaningful third job type here (a saved URL in `params`).
export type ScheduledTaskJobType = AutomationJobType | "DOWNLOAD";

export interface ScheduledTaskConfig {
  id: string;
  name: string;
  cronExpression: string;
  jobType: ScheduledTaskJobType;
  params: Record<string, unknown>;
  enabled: boolean;
}
