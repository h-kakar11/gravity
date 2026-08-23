// The TypeScript half of docs/ipc-contract.md. services/coreClient.ts is the only module
// allowed to construct these request/event shapes by hand -- everything else in the
// frontend calls its typed methods instead of touching the wire format directly.

import type { JobSnapshot, JobType } from "./job";
import type { FileInfo } from "./fileInfo";
import type { HardwareInfo } from "./hardware";
import type { Settings } from "./settings";
import type { ErrorInfo } from "./error";
import type { Progress } from "./progress";
import type { DownloadMetadata, QualityPreset } from "./download";

export type CoreCommand =
  | "createJob"
  | "getJob"
  | "listJobs"
  | "cancelJob"
  | "pauseJob"
  | "resumeJob"
  | "retryJob"
  | "inspectFile"
  | "inspectDownloadUrl"
  | "getCapabilities"
  | "getSettings"
  | "updateSettings"
  | "getHardwareInfo";

// Params a DOWNLOAD-type createJob call takes, nested under CommandParams["createJob"].params
// (see docs/ipc-contract.md "createJob params by type"). `quality` defaults to "BEST" on
// the C++ side when omitted.
export interface DownloadJobParams {
  url: string;
  outputDirectory: string;
  quality?: QualityPreset;
}

// Params for each command, keyed by command name.
export interface CommandParams {
  createJob: { type: JobType; params: Record<string, unknown> };
  getJob: { jobId: string };
  listJobs: Record<string, never>;
  cancelJob: { jobId: string };
  pauseJob: { jobId: string };
  resumeJob: { jobId: string };
  retryJob: { jobId: string };
  inspectFile: { path: string };
  inspectDownloadUrl: { url: string };
  getCapabilities: { path: string };
  getSettings: Record<string, never>;
  updateSettings: { settings: Partial<Settings> };
  getHardwareInfo: Record<string, never>;
}

// Result for each command, keyed by command name.
export interface CommandResult {
  createJob: { jobId: string };
  getJob: { job: JobSnapshot };
  listJobs: { jobs: JobSnapshot[] };
  cancelJob: Record<string, never>;
  pauseJob: Record<string, never>;
  resumeJob: Record<string, never>;
  retryJob: Record<string, never>;
  inspectFile: { fileInfo: FileInfo };
  inspectDownloadUrl: { metadata: DownloadMetadata };
  getCapabilities: { capabilities: string[] };
  getSettings: { settings: Settings };
  updateSettings: { settings: Settings };
  getHardwareInfo: { hardwareInfo: HardwareInfo };
}

// Envelope written to mediatool-core's stdin (via the Tauri `send_core_command` command).
export interface CoreRequest<C extends CoreCommand = CoreCommand> {
  id: string;
  command: C;
  params: CommandParams[C];
}

// Envelope read back from mediatool-core's stdout, forwarded by Rust as a Tauri event
// named "core-response".
export type CoreResponse<C extends CoreCommand = CoreCommand> =
  | { id: string; ok: true; result: CommandResult[C] }
  | { id: string; ok: false; error: ErrorInfo };

export type CoreEventName =
  | "jobCreated"
  | "jobQueued"
  | "jobStarted"
  | "jobProgress"
  | "jobPaused"
  | "jobResumed"
  | "jobCompleted"
  | "jobFailed"
  | "jobCancelled"
  | "fileDetected"
  | "hardwareDetected"
  | "downloadMetadataReceived"
  | "logEvent";

// Data payload per event name. Job lifecycle events always carry at least `{state}`;
// jobProgress additionally carries the full Progress object.
export interface CoreEventData {
  jobCreated: { state: "QUEUED" };
  jobQueued: { state: "QUEUED" };
  jobStarted: { state: "STARTING" | "RUNNING" };
  jobProgress: { state: "RUNNING" } & Progress;
  jobPaused: { state: "PAUSED" };
  jobResumed: { state: "RUNNING" };
  jobCompleted: { state: "COMPLETED"; result?: Record<string, unknown> };
  jobFailed: { state: "FAILED"; error: ErrorInfo };
  jobCancelled: { state: "CANCELLED" };
  fileDetected: { fileInfo: FileInfo };
  hardwareDetected: { hardwareInfo: HardwareInfo };
  downloadMetadataReceived: {
    jobId: string;
    title: string;
    durationSeconds?: number;
    playlistIndex?: number;
    playlistCount?: number;
  };
  logEvent: { level: "DEBUG" | "INFO" | "WARNING" | "ERROR"; message: string; subsystem: string };
}

// Raw shape forwarded by Rust as the Tauri event "core-event".
export interface CoreEvent<E extends CoreEventName = CoreEventName> {
  event: E;
  jobId?: string;
  timestamp: string;
  data: CoreEventData[E];
}
