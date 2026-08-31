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
import type { Preset, PresetKind } from "./preset";

export type CoreCommand =
  | "createJob"
  | "getJob"
  | "listJobs"
  | "listJobHistory"
  | "cancelJob"
  | "pauseJob"
  | "resumeJob"
  | "retryJob"
  | "removeJob"
  | "inspectFile"
  | "inspectDownloadUrl"
  | "getCapabilities"
  | "getSettings"
  | "updateSettings"
  | "getHardwareInfo"
  | "getMediaEngineCapabilities"
  | "listPresets"
  | "savePreset"
  | "deletePreset";

// Params a DOWNLOAD-type createJob call takes, nested under CommandParams["createJob"].params
// (see docs/ipc-contract.md "createJob params by type"). `quality` defaults to "BEST" on
// the C++ side when omitted. `formatId` (an exact stream id from InspectDownloadUrl's
// DownloadFormat list, or an "id1+id2" combo) overrides `quality` entirely when set --
// issue #31.
export interface DownloadJobParams {
  url: string;
  outputDirectory: string;
  quality?: QualityPreset;
  formatId?: string;
  // Scheduling priority (issue #17): higher runs before lower among jobs still Queued.
  // Omitted/0 keeps plain-FIFO ordering. Must be within [-1000, 1000].
  priority?: number;
  // Ids of jobs that must COMPLETE before this one starts (issue #17). Every id must be a
  // job the core already knows about; a job whose dependency does not complete is
  // cancelled. See docs/ipc-contract.md "Scheduling params" and docs/concurrency-model.md.
  dependsOn?: string[];
}

// Params for each command, keyed by command name.
export interface CommandParams {
  createJob: { type: JobType; params: Record<string, unknown> };
  getJob: { jobId: string };
  listJobs: Record<string, never>;
  listJobHistory: { limit?: number };
  cancelJob: { jobId: string };
  pauseJob: { jobId: string };
  resumeJob: { jobId: string };
  retryJob: { jobId: string };
  // Only a terminal (Completed/Failed/Cancelled) job can be removed -- the core rejects
  // anything else with E_INVALID_OPERATION.
  removeJob: { jobId: string };
  inspectFile: { path: string };
  inspectDownloadUrl: { url: string };
  getCapabilities: { path: string };
  getSettings: Record<string, never>;
  updateSettings: { settings: Partial<Settings> };
  getHardwareInfo: Record<string, never>;
  getMediaEngineCapabilities: Record<string, never>;
  listPresets: Record<string, never>;
  // `id` present means "update this existing preset"; omitted means "create a new one".
  savePreset: { id?: string; name: string; kind: PresetKind; options: Record<string, unknown> };
  deletePreset: { id: string };
}

// Result for each command, keyed by command name.
export interface CommandResult {
  createJob: { jobId: string };
  getJob: { job: JobSnapshot };
  listJobs: { jobs: JobSnapshot[] };
  listJobHistory: { jobs: JobSnapshot[] };
  cancelJob: Record<string, never>;
  pauseJob: Record<string, never>;
  resumeJob: Record<string, never>;
  retryJob: Record<string, never>;
  removeJob: Record<string, never>;
  inspectFile: { fileInfo: FileInfo };
  inspectDownloadUrl: { metadata: DownloadMetadata };
  getCapabilities: { capabilities: string[] };
  getSettings: { settings: Settings };
  updateSettings: { settings: Settings };
  getHardwareInfo: { hardwareInfo: HardwareInfo };
  getMediaEngineCapabilities: {
    availableEncoders: string[];
    hardwareEncodersAvailable: { nvenc: boolean; amf: boolean; qsv: boolean };
  };
  listPresets: { presets: Preset[] };
  savePreset: { preset: Preset };
  deletePreset: Record<string, never>;
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
