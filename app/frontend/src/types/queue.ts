// Mirrors core/queue/QueueTypes.h and JobManager::QueueSnapshot.

import type { JobSnapshot } from "./job";

export type QueueRunState = "RUNNING" | "PAUSED";

export type MoveDirection = "TOP" | "UP" | "DOWN" | "BOTTOM";

export type HistoryScope = "COMPLETED" | "FAILED" | "CANCELLED" | "SKIPPED" | "ALL";

// Aggregate counts. Deliberately carries no overall percentage: a download measured in
// bytes and an encode measured in seconds share no denominator, and averaging them would
// invent a number (spec section 33).
export interface QueueStatistics {
  running: number;
  queued: number;
  waiting: number;
  retryWait: number;
  paused: number;
  completed: number;
  failed: number;
  cancelled: number;
  skipped: number;
  total: number;
}

export interface QueueSnapshot {
  runState: QueueRunState;
  maxConcurrency: number;
  statistics: QueueStatistics;
  jobs: JobSnapshot[];
  // The backend's authoritative scheduling order for pending jobs.
  pendingOrder: string[];
  // Every event up to and including this sequence number is already reflected here.
  sequence: number;
}

export const EMPTY_STATISTICS: QueueStatistics = {
  running: 0,
  queued: 0,
  waiting: 0,
  retryWait: 0,
  paused: 0,
  completed: 0,
  failed: 0,
  cancelled: 0,
  skipped: 0,
  total: 0,
};
