// The TypeScript half of docs/ipc-contract.md. services/coreClient.ts is the only module
// allowed to construct these request/event shapes by hand -- everything else in the
// frontend calls its typed methods instead of touching the wire format directly.

import type { JobPriority, JobSnapshot, JobState, JobType } from "./job";
import type { HistoryScope, MoveDirection, QueueRunState, QueueSnapshot } from "./queue";
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
  | "removeJob"
  | "getQueueSnapshot"
  | "setJobPriority"
  | "moveJob"
  | "pauseQueue"
  | "resumeQueue"
  | "setConcurrency"
  | "clearHistory"
  | "retryFailedJobs"
  | "getProcessingCapabilities"
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

// The closed set of containers a conversion can target. Kept in sync with
// core/media/ProcessingOptions.h; getProcessingCapabilities returns the backend's own copy
// so a picker can be built from truth rather than from this list.
export type TargetFormat =
  | "MP4"
  | "MKV"
  | "WEBM"
  | "MOV"
  | "GIF"
  | "MP3"
  | "WAV"
  | "M4A"
  | "FLAC"
  | "OPUS";

export type CompressionPreset = "LOW" | "MEDIUM" | "HIGH";

// Exactly one of `inputPath` and `inputFromJobId` must be given.
//
// `inputFromJobId` is what makes a real pipeline possible: the path a producing job writes
// is not knowable in advance -- a download's filename comes from the media's title and
// whichever container the extractor chose -- so a following stage names the job it reads
// from and the backend resolves the actual path once that job has run. Declaring it also
// creates the dependency, so the stage cannot start early (spec section 19).
interface ProcessingInput {
  inputPath?: string;
  inputFromJobId?: string;
}

export interface ConversionJobParams extends ProcessingInput {
  outputDirectory: string;
  targetFormat: TargetFormat;
  outputFilenameBase?: string;
  audioBitrateKbps?: number;
  gifFps?: number;
  maxHeight?: number;
}

export interface CompressionJobParams extends ProcessingInput {
  outputDirectory: string;
  preset?: CompressionPreset;
  outputFilenameBase?: string;
  // Compression keeps the source container by default. When the input comes from another
  // job the source extension is not known yet, so this is the only way to say anything
  // other than mp4.
  outputExtension?: string;
  maxHeight?: number;
  audioBitrateKbps?: number;
}

// Scheduling options every createJob call accepts, independent of job type.
export interface CreateJobScheduling {
  priority?: JobPriority;
  dependsOn?: string[];
  parentJobId?: string;
  allowDuplicate?: boolean;
  retryPolicy?: {
    maxRetries?: number;
    initialDelayMs?: number;
    maxDelayMs?: number;
    multiplier?: number;
  };
}

// Params for each command, keyed by command name.
export interface CommandParams {
  createJob: { type: JobType; params: Record<string, unknown> } & CreateJobScheduling;
  getJob: { jobId: string };
  listJobs: Record<string, never>;
  cancelJob: { jobId: string };
  pauseJob: { jobId: string };
  resumeJob: { jobId: string };
  retryJob: { jobId: string };
  removeJob: { jobId: string };
  getQueueSnapshot: Record<string, never>;
  setJobPriority: { jobId: string; priority: JobPriority };
  moveJob: { jobId: string; direction: MoveDirection };
  pauseQueue: Record<string, never>;
  resumeQueue: Record<string, never>;
  setConcurrency: { maxConcurrency: number };
  clearHistory: { scope: HistoryScope };
  retryFailedJobs: Record<string, never>;
  getProcessingCapabilities: Record<string, never>;
  inspectFile: { path: string };
  inspectDownloadUrl: { url: string };
  getCapabilities: { path: string };
  getSettings: Record<string, never>;
  updateSettings: { settings: Partial<Settings> };
  getHardwareInfo: Record<string, never>;
}

// Result for each command, keyed by command name.
export interface CommandResult {
  createJob: { jobId: string; duplicateKey: string };
  getJob: { job: JobSnapshot };
  listJobs: { jobs: JobSnapshot[] };
  cancelJob: Record<string, never>;
  pauseJob: Record<string, never>;
  resumeJob: Record<string, never>;
  retryJob: Record<string, never>;
  removeJob: Record<string, never>;
  getQueueSnapshot: { queue: QueueSnapshot };
  setJobPriority: Record<string, never>;
  moveJob: Record<string, never>;
  pauseQueue: { runState: QueueRunState };
  resumeQueue: { runState: QueueRunState };
  setConcurrency: { maxConcurrency: number };
  clearHistory: { removedJobIds: string[]; removedCount: number };
  retryFailedJobs: { retriedJobIds: string[]; retriedCount: number };
  getProcessingCapabilities: {
    targetFormats: TargetFormat[];
    compressionPresets: CompressionPreset[];
    priorities: JobPriority[];
    ffmpegAvailable: boolean;
  };
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
  | "jobSkipped"
  | "jobRetryScheduled"
  | "queueChanged"
  | "fileDetected"
  | "hardwareDetected"
  | "downloadMetadataReceived"
  | "logEvent";

// Fields every job lifecycle event carries. `revision` is the job's own monotonic counter;
// combined with the envelope's `seq` it gives two independent ways to reject a stale update.
interface JobEventBase {
  state: JobState;
  revision?: number;
}

// Data payload per event name.
export interface CoreEventData {
  jobCreated: JobEventBase & { type?: JobType };
  jobQueued: JobEventBase;
  jobStarted: JobEventBase;
  jobProgress: { state: "RUNNING" } & Progress;
  jobPaused: JobEventBase;
  jobResumed: JobEventBase;
  jobCompleted: JobEventBase & { result?: Record<string, unknown> };
  jobFailed: JobEventBase & { error: ErrorInfo };
  jobCancelled: JobEventBase;
  jobSkipped: JobEventBase & { error?: ErrorInfo };
  jobRetryScheduled: JobEventBase & {
    attempt: number;
    delayMs: number;
    reason: string;
    maxRetries?: number;
    nextRetryAtMs?: number;
    error?: ErrorInfo;
  };
  queueChanged: {
    runState: QueueRunState;
    maxConcurrency: number;
    statistics: import("./queue").QueueStatistics;
    pendingOrder: string[];
  };
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
  // Monotonic per core process, stamped as the line is written, so sequence order and
  // arrival order are the same thing (spec section 57).
  seq: number;
  data: CoreEventData[E];
}

// Every event whose payload describes one specific job's lifecycle.
export const JOB_LIFECYCLE_EVENTS: ReadonlySet<CoreEventName> = new Set<CoreEventName>([
  "jobCreated",
  "jobQueued",
  "jobStarted",
  "jobProgress",
  "jobPaused",
  "jobResumed",
  "jobCompleted",
  "jobFailed",
  "jobCancelled",
  "jobSkipped",
  "jobRetryScheduled",
]);
