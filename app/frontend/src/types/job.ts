// Mirrors core/jobs/JobTypes.h and docs/ipc-contract.md exactly. Wire values are
// UPPER_SNAKE_CASE strings -- do not translate casing on this side of the boundary.

export type JobType =
  | "DOWNLOAD"
  | "CONVERSION"
  | "COMPRESSION"
  | "BATCH"
  | "WORKFLOW"
  // Phase-1-only synthetic job used to prove the pipeline end-to-end.
  | "TEST";

export type JobState =
  | "QUEUED"
  | "STARTING"
  | "RUNNING"
  | "PAUSED"
  | "COMPLETED"
  | "FAILED"
  | "CANCELLED"
  | "RETRYING";

import type { Progress } from "./progress";
import type { ErrorInfo } from "./error";

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
}
