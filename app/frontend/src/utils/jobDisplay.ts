// Turns backend facts into things a person can read (spec sections 21, 33, 36).
//
// The rule the whole file follows: the backend sends structured codes and typed fields, and
// the frontend maps them to words. Nothing here parses stderr, and nothing here invents
// information the backend did not send.

import type { ErrorInfo } from "../types/error";
import type { JobPriority, JobSnapshot, JobState, JobType } from "../types/job";

// --- units ------------------------------------------------------------------------------

export function formatBytes(bytes: number | undefined): string {
  if (bytes === undefined || !Number.isFinite(bytes)) return "--";
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(0)} KB`;
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} GB`;
}

export function formatSpeed(bytesPerSecond: number | undefined): string {
  if (bytesPerSecond === undefined || !Number.isFinite(bytesPerSecond)) return "--";
  return `${formatBytes(bytesPerSecond)}/s`;
}

export function formatDuration(seconds: number | undefined): string {
  if (seconds === undefined || !Number.isFinite(seconds) || seconds < 0) return "--";
  const total = Math.round(seconds);
  const hh = Math.floor(total / 3600);
  const mm = Math.floor((total % 3600) / 60);
  const ss = total % 60;
  if (hh > 0) return `${hh}:${mm.toString().padStart(2, "0")}:${ss.toString().padStart(2, "0")}`;
  return `${mm}:${ss.toString().padStart(2, "0")}`;
}

export function formatTimestamp(iso: string | undefined): string {
  if (!iso) return "--";
  const parsed = new Date(iso);
  if (Number.isNaN(parsed.getTime())) return iso;
  return parsed.toLocaleString();
}

// Keeps a long filename or URL from destroying the layout while still showing both ends,
// which is where the distinguishing part of a path or URL usually is (spec section 37).
export function truncateMiddle(value: string, max = 64): string {
  if (value.length <= max) return value;
  const keep = Math.floor((max - 1) / 2);
  return `${value.slice(0, keep)}…${value.slice(value.length - keep)}`;
}

// --- labels -------------------------------------------------------------------------------

export const JOB_STATE_LABELS: Record<JobState, string> = {
  QUEUED: "Queued",
  WAITING: "Waiting for dependency",
  STARTING: "Starting",
  RUNNING: "Running",
  PAUSED: "Paused",
  RETRY_WAIT: "Retrying soon",
  COMPLETED: "Completed",
  FAILED: "Failed",
  CANCELLED: "Cancelled",
  SKIPPED: "Skipped",
  RETRYING: "Retrying",
};

export const JOB_TYPE_LABELS: Record<JobType, string> = {
  DOWNLOAD: "Download",
  CONVERSION: "Convert",
  COMPRESSION: "Compress",
  BATCH: "Batch",
  WORKFLOW: "Workflow",
  TEST: "Test",
};

export const PRIORITY_LABELS: Record<JobPriority, string> = {
  LOW: "Low",
  NORMAL: "Normal",
  HIGH: "High",
};

// --- error presentation ---------------------------------------------------------------------

// Backend error code -> a sentence a person can act on. The backend's own `message` is
// already user-facing, so this only overrides it where the frontend can say something more
// useful (spec section 36). Anything unmapped falls back to the backend's message, never to
// a raw code.
const ERROR_MESSAGES: Record<string, string> = {
  E_INPUT_NOT_FOUND: "The input file could not be found. It may have been moved or deleted.",
  E_OUTPUT_MISSING: "Processing finished but produced no file.",
  E_OUTPUT_EMPTY: "Processing produced an empty file.",
  E_OUTPUT_VERIFICATION_FAILED: "The produced file failed verification and was discarded.",
  E_SAME_INPUT_OUTPUT: "The output would overwrite the input file. Choose a different folder or name.",
  E_FFMPEG_NOT_FOUND: "FFmpeg was not found on this system. Install it or set its path in Settings.",
  E_FFMPEG_FAILED: "FFmpeg could not process this file.",
  E_FFMPEG_STALLED: "FFmpeg stopped responding and was terminated.",
  E_FFMPEG_LAUNCH_FAILED: "FFmpeg could not be started.",
  E_INVALID_DOWNLOAD_URL: "That is not a supported media URL.",
  E_DOWNLOAD_OUTPUT_MISSING: "The download reported success but produced no file.",
  E_DOWNLOAD_OUTPUT_EMPTY: "The downloaded file is empty.",
  E_DOWNLOAD_VERIFICATION_FAILED: "The downloaded file failed verification.",
  E_INSUFFICIENT_DISK_SPACE: "Not enough free space in the output folder.",
  E_DEPENDENCY_FAILED: "Skipped because a job it depended on did not finish.",
  E_DUPLICATE_JOB: "An identical job is already in the queue.",
  E_JOB_INTERRUPTED: "This job was interrupted when the app closed. Retry to run it again.",
  E_JOB_NOT_FOUND: "That job is no longer in the queue.",
  E_JOB_INVALID_OPERATION: "That action is not available for this job right now.",
  E_INVALID_PROCESSING_OPTION: "One of the chosen options is not valid.",
  E_UNSUPPORTED_TARGET_FORMAT: "That output format is not supported.",
  E_INVALID_OUTPUT_PATH: "The output location is not usable.",
  E_OUTPUT_DIRECTORY_UNUSABLE: "The output folder could not be created.",
  E_PROCESSING_CANCELLED: "Cancelled.",
  E_DOWNLOAD_JOB_CANCELLED: "Cancelled.",
};

export function describeError(error: ErrorInfo | undefined): string {
  if (!error) return "";
  return ERROR_MESSAGES[error.code] ?? error.message;
}

// --- job description -------------------------------------------------------------------------

function metadataString(job: JobSnapshot, key: string): string | undefined {
  const value = job.metadata?.[key];
  return typeof value === "string" && value.length > 0 ? value : undefined;
}

function metadataNumber(job: JobSnapshot, key: string): number | undefined {
  const value = job.metadata?.[key];
  return typeof value === "number" ? value : undefined;
}

// The one line that identifies a job in the list. Falls back progressively rather than
// showing an opaque id: a download shows its title, then its URL; a local job shows its
// input filename.
export function jobTitle(job: JobSnapshot): string {
  const title = metadataString(job, "title");
  if (title) return title;
  const inputFilename = metadataString(job, "inputFilename");
  if (inputFilename) return inputFilename;
  const webpageUrl = metadataString(job, "webpageUrl");
  if (webpageUrl) return truncateMiddle(webpageUrl, 72);
  const outputFilename = metadataString(job, "outputFilename");
  if (outputFilename) return outputFilename;
  return job.id;
}

// The second line: what this job will actually do, in the job type's own terms
// (spec section 21). Never exposes an ffmpeg command line.
export function jobSubtitle(job: JobSnapshot): string {
  const parts: string[] = [];
  switch (job.type) {
    case "DOWNLOAD": {
      const quality = metadataString(job, "quality");
      if (quality) parts.push(quality);
      const uploader = metadataString(job, "uploader");
      if (uploader) parts.push(uploader);
      break;
    }
    case "CONVERSION": {
      const source = metadataString(job, "sourceFormat");
      const target = metadataString(job, "targetFormat");
      if (source && target) parts.push(`${source.toUpperCase()} → ${target.toUpperCase()}`);
      else if (target) parts.push(target.toUpperCase());
      const maxHeight = metadataNumber(job, "maxHeight");
      if (maxHeight) parts.push(`max ${maxHeight}p`);
      break;
    }
    case "COMPRESSION": {
      const preset = metadataString(job, "preset");
      if (preset) parts.push(`${PRIORITY_LABELS[preset as JobPriority] ?? preset} quality`);
      const maxHeight = metadataNumber(job, "maxHeight");
      if (maxHeight) parts.push(`max ${maxHeight}p`);
      break;
    }
    default:
      break;
  }
  const outputFilename = metadataString(job, "outputFilename");
  if (outputFilename) parts.push(`→ ${truncateMiddle(outputFilename, 40)}`);
  return parts.join(" · ");
}

// --- control availability ------------------------------------------------------------------
// A control that cannot work is disabled, never hidden and never shown as if it would work
// (spec section 31). These predicates are the single definition of "can", shared by the
// row's buttons and the detail panel's.

export function canCancel(job: JobSnapshot): boolean {
  return !["COMPLETED", "FAILED", "CANCELLED", "SKIPPED"].includes(job.state);
}

export function canRetry(job: JobSnapshot): boolean {
  return job.state === "FAILED" || job.state === "RETRY_WAIT" || job.state === "SKIPPED";
}

// Only a job that has not started can be reordered; a running or finished job has no queue
// position to change.
export function canReorder(job: JobSnapshot): boolean {
  return job.queuePosition !== undefined;
}

export function canChangePriority(job: JobSnapshot): boolean {
  return !["COMPLETED", "FAILED", "CANCELLED", "SKIPPED"].includes(job.state);
}

export function canRemove(job: JobSnapshot): boolean {
  return ["COMPLETED", "FAILED", "CANCELLED", "SKIPPED"].includes(job.state);
}

// How long until an automatic retry fires, in seconds. Undefined when no retry is pending.
export function secondsUntilRetry(job: JobSnapshot, nowMs: number): number | undefined {
  if (job.state !== "RETRY_WAIT" || job.nextRetryAtMs === undefined) return undefined;
  return Math.max(0, Math.round((job.nextRetryAtMs - nowMs) / 1000));
}
