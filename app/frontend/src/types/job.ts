// Mirrors core/jobs/JobTypes.h, core/queue/QueueTypes.h and docs/ipc-contract.md exactly.
// Wire values are UPPER_SNAKE_CASE strings -- do not translate casing on this side of the
// boundary.

export type JobType =
  | "DOWNLOAD"
  | "CONVERSION"
  | "COMPRESSION"
  | "BATCH"
  | "WORKFLOW"
  // Synthetic job used to prove the pipeline end-to-end; not a user-facing feature.
  | "TEST";

export type JobState =
  | "QUEUED"
  // Blocked: at least one dependency has not completed successfully yet.
  | "WAITING"
  | "STARTING"
  | "RUNNING"
  | "PAUSED"
  // An automatic retry is scheduled; the job runs again once its backoff elapses.
  | "RETRY_WAIT"
  | "COMPLETED"
  | "FAILED"
  | "CANCELLED"
  // A dependency failed or was cancelled, so this job will never run.
  | "SKIPPED"
  | "RETRYING";

export type JobPriority = "LOW" | "NORMAL" | "HIGH";

// States in which a job is still live in the queue.
export const ACTIVE_STATES: ReadonlySet<JobState> = new Set<JobState>([
  "QUEUED",
  "WAITING",
  "STARTING",
  "RUNNING",
  "PAUSED",
  "RETRY_WAIT",
  "RETRYING",
]);

// States in which a job holds one of the scheduler's concurrency slots.
export const EXECUTING_STATES: ReadonlySet<JobState> = new Set<JobState>([
  "STARTING",
  "RUNNING",
  "PAUSED",
  "RETRYING",
]);

// States in which a job is pending: still live, but holding no slot.
export const PENDING_STATES: ReadonlySet<JobState> = new Set<JobState>([
  "QUEUED",
  "WAITING",
  "RETRY_WAIT",
]);

export const TERMINAL_STATES: ReadonlySet<JobState> = new Set<JobState>([
  "COMPLETED",
  "FAILED",
  "CANCELLED",
  "SKIPPED",
]);

import type { Progress } from "./progress";
import type { ErrorInfo } from "./error";

// The unified job model (spec section 5). Every job type -- download, conversion,
// compression -- arrives in exactly this shape, so no component has to branch on job type
// just to read state, progress, or scheduling fields.
export interface JobSnapshot {
  id: string;
  type: JobType;
  state: JobState;
  createdAt: string;
  startedAt?: string;
  completedAt?: string;
  progress: Progress;
  error?: ErrorInfo;
  result?: Record<string, unknown>;
  metadata?: Record<string, unknown>;

  priority: JobPriority;
  // Number of attempts beyond the first. `retryCount` is the same number under the name
  // the spec uses; both are sent so neither name has to win.
  attempt: number;
  retryCount: number;
  maxRetries: number;
  nextRetryAtMs?: number;
  retryReason?: string;
  dependencies: string[];
  parentJobId?: string;
  // Position in the backend's pending order, absent when the job is not pending.
  queuePosition?: number;
  // Increments on every durable change. Used to discard stale updates (spec section 57).
  revision: number;
}
