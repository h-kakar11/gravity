// Shared builders for component tests: a minimal valid JobSnapshot and a QueueController
// whose command methods are vi.fn() spies, so a test can assert what a click actually sent
// to the backend rather than only what the DOM shows.

import { vi } from "vitest";
import type { JobSnapshot } from "../types/job";
import type { QueueController } from "../state/useQueue";
import { EMPTY_STATISTICS } from "../types/queue";
import { initialQueueState, type QueueState } from "../state/queueReducer";

export function makeJob(overrides: Partial<JobSnapshot> = {}): JobSnapshot {
  return {
    id: "job-1",
    type: "DOWNLOAD",
    state: "QUEUED",
    createdAt: "2026-01-01T00:00:00.000Z",
    progress: { statusMessage: "" },
    priority: "NORMAL",
    attempt: 0,
    retryCount: 0,
    maxRetries: 0,
    dependencies: [],
    revision: 1,
    ...overrides,
  };
}

export function makeQueueState(overrides: Partial<QueueState> = {}): QueueState {
  return {
    ...initialQueueState,
    loaded: true,
    statistics: { ...EMPTY_STATISTICS },
    ...overrides,
  };
}

export function makeQueueController(overrides: Partial<QueueController> = {}): QueueController {
  const state = overrides.state ?? makeQueueState();
  return {
    state,
    jobs: overrides.jobs ?? Object.values(state.jobs),
    actionError: overrides.actionError ?? null,
    clearActionError: vi.fn(),
    refresh: vi.fn().mockResolvedValue(undefined),
    cancelJob: vi.fn().mockResolvedValue(undefined),
    retryJob: vi.fn().mockResolvedValue(undefined),
    removeJob: vi.fn().mockResolvedValue(undefined),
    setPriority: vi.fn().mockResolvedValue(undefined),
    moveJob: vi.fn().mockResolvedValue(undefined),
    pauseQueue: vi.fn().mockResolvedValue(undefined),
    resumeQueue: vi.fn().mockResolvedValue(undefined),
    setConcurrency: vi.fn().mockResolvedValue(undefined),
    clearHistory: vi.fn().mockResolvedValue(undefined),
    retryFailed: vi.fn().mockResolvedValue(undefined),
    ...overrides,
  };
}
