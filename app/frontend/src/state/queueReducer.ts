// The single source of truth for queue state in the frontend (spec section 56).
//
// Deliberately a pure reducer with no React in it: given a state and an event, it returns
// the next state. That makes every reconciliation rule -- out-of-order events, unknown
// jobs, reconnects, duplicate deliveries -- testable as plain function calls, and it means
// no component can invent its own idea of what a job's state is. Components read from this
// store; only the backend writes to it.
//
// Reconciliation model (spec section 57). The backend is authoritative. Two independent
// guards keep a late event from clobbering newer state:
//
//   1. `seq` -- monotonic per core process, stamped as the event line is written, so
//      sequence order IS wire order. An event whose seq is not newer than the last one
//      applied is dropped outright.
//   2. `revision` -- the job's own counter, incremented on every durable backend change.
//      An event carrying an older revision than the job we already hold is dropped even if
//      its seq is newer, which covers the case where two jobs' events interleave.
//
// Progress events carry no revision (progress is volatile and deliberately not persisted),
// so they are applied on seq alone, and never to a job that has already finished.

import type { CoreEvent, CoreEventName } from "../types/ipc";
import type { JobSnapshot, JobState } from "../types/job";
import { TERMINAL_STATES } from "../types/job";
import type { QueueRunState, QueueSnapshot, QueueStatistics } from "../types/queue";
import { EMPTY_STATISTICS } from "../types/queue";

export interface QueueState {
  jobs: Record<string, JobSnapshot>;
  // Backend's authoritative pending order. The UI may sort its view differently, but this
  // is what the scheduler will actually do.
  pendingOrder: string[];
  runState: QueueRunState;
  maxConcurrency: number;
  statistics: QueueStatistics;
  // Highest event sequence applied so far.
  lastSequence: number;
  // True once a snapshot has been loaded; until then the UI shows "connecting" rather than
  // an empty queue, which would be a lie.
  loaded: boolean;
  // Counts events discarded as stale. Surfaced in the dev console, and asserted in tests.
  droppedEvents: number;
}

export const initialQueueState: QueueState = {
  jobs: {},
  pendingOrder: [],
  runState: "RUNNING",
  maxConcurrency: 1,
  statistics: EMPTY_STATISTICS,
  lastSequence: 0,
  loaded: false,
  droppedEvents: 0,
};

export type QueueAction =
  | { type: "snapshot"; snapshot: QueueSnapshot }
  | { type: "event"; event: CoreEvent }
  | { type: "disconnected" };

// Which JobState a lifecycle event implies, when the payload does not spell it out. The
// payload's own `state` always wins; this is the fallback for a malformed or older event.
const EVENT_FALLBACK_STATE: Partial<Record<CoreEventName, JobState>> = {
  jobCreated: "QUEUED",
  jobQueued: "QUEUED",
  jobStarted: "RUNNING",
  jobPaused: "PAUSED",
  jobResumed: "RUNNING",
  jobCompleted: "COMPLETED",
  jobFailed: "FAILED",
  jobCancelled: "CANCELLED",
  jobSkipped: "SKIPPED",
  jobRetryScheduled: "RETRY_WAIT",
};

function applySnapshot(state: QueueState, snapshot: QueueSnapshot): QueueState {
  const jobs: Record<string, JobSnapshot> = {};
  for (const job of snapshot.jobs) jobs[job.id] = job;
  return {
    jobs,
    pendingOrder: snapshot.pendingOrder,
    runState: snapshot.runState,
    maxConcurrency: snapshot.maxConcurrency,
    statistics: snapshot.statistics,
    // A snapshot replaces everything, so anything at or below its sequence is already
    // accounted for. The backend reads this counter before building the snapshot, so at
    // worst the frontend re-applies an event it already has -- harmless, because every
    // event assigns state rather than mutating it incrementally.
    lastSequence: snapshot.sequence,
    loaded: true,
    droppedEvents: state.droppedEvents,
  };
}

function applyQueueChanged(state: QueueState, event: CoreEvent<"queueChanged">): QueueState {
  return {
    ...state,
    runState: event.data.runState,
    maxConcurrency: event.data.maxConcurrency,
    statistics: event.data.statistics,
    pendingOrder: event.data.pendingOrder,
    lastSequence: event.seq,
  };
}

function applyProgress(state: QueueState, event: CoreEvent<"jobProgress">): QueueState {
  const jobId = event.jobId;
  if (!jobId) return { ...state, lastSequence: event.seq };
  const existing = state.jobs[jobId];
  // Progress for a job we have never seen, or one that has already finished, is stale by
  // definition -- a finished job's progress bar must not start moving again.
  if (!existing || TERMINAL_STATES.has(existing.state)) {
    return { ...state, lastSequence: event.seq };
  }
  const { state: _ignored, ...progress } = event.data;
  return {
    ...state,
    jobs: { ...state.jobs, [jobId]: { ...existing, progress } },
    lastSequence: event.seq,
  };
}

function applyJobLifecycle(state: QueueState, event: CoreEvent): QueueState {
  const jobId = event.jobId;
  if (!jobId) return { ...state, lastSequence: event.seq };

  const data = event.data as {
    state?: JobState;
    revision?: number;
    error?: JobSnapshot["error"];
    result?: Record<string, unknown>;
    attempt?: number;
    maxRetries?: number;
    nextRetryAtMs?: number;
    reason?: string;
  };
  const existing = state.jobs[jobId];

  if (existing && data.revision !== undefined && data.revision < existing.revision) {
    // A late event about a job we already have fresher information for.
    return { ...state, lastSequence: event.seq, droppedEvents: state.droppedEvents + 1 };
  }

  const nextState = data.state ?? EVENT_FALLBACK_STATE[event.event] ?? existing?.state;
  if (!nextState) return { ...state, lastSequence: event.seq };

  // A job the frontend has never seen -- it was created in another window, or this client
  // connected mid-flight. Build a minimal record from the event; the next snapshot fills
  // in the rest rather than leaving the job invisible.
  const base: JobSnapshot = existing ?? {
    id: jobId,
    type: "TEST",
    state: nextState,
    createdAt: event.timestamp,
    progress: { statusMessage: "" },
    priority: "NORMAL",
    attempt: 0,
    retryCount: 0,
    maxRetries: 0,
    dependencies: [],
    revision: 0,
  };

  const updated: JobSnapshot = {
    ...base,
    state: nextState,
    revision: data.revision ?? base.revision,
  };
  if (data.error !== undefined) updated.error = data.error;
  if (data.result !== undefined) updated.result = data.result;
  if (data.attempt !== undefined) {
    updated.attempt = data.attempt;
    updated.retryCount = data.attempt;
  }
  if (data.maxRetries !== undefined) updated.maxRetries = data.maxRetries;
  if (data.nextRetryAtMs !== undefined) updated.nextRetryAtMs = data.nextRetryAtMs;
  if (data.reason !== undefined) updated.retryReason = data.reason;
  if (event.event === "jobCompleted" || event.event === "jobFailed") {
    updated.completedAt = updated.completedAt ?? event.timestamp;
  }
  if (event.event === "jobStarted" && !updated.startedAt) updated.startedAt = event.timestamp;

  return {
    ...state,
    jobs: { ...state.jobs, [jobId]: updated },
    lastSequence: event.seq,
  };
}

export function queueReducer(state: QueueState, action: QueueAction): QueueState {
  switch (action.type) {
    case "snapshot":
      return applySnapshot(state, action.snapshot);

    case "disconnected":
      // Keep the jobs on screen but mark the store unloaded, so the UI can say the
      // connection dropped instead of silently showing state that has stopped updating.
      return { ...state, loaded: false };

    case "event": {
      const { event } = action;

      // Guard 1: sequence. Applies to every event including progress and queueChanged.
      if (event.seq !== undefined && event.seq <= state.lastSequence) {
        return { ...state, droppedEvents: state.droppedEvents + 1 };
      }

      if (event.event === "queueChanged") {
        return applyQueueChanged(state, event as CoreEvent<"queueChanged">);
      }
      if (event.event === "jobProgress") {
        return applyProgress(state, event as CoreEvent<"jobProgress">);
      }
      if (EVENT_FALLBACK_STATE[event.event] !== undefined) {
        return applyJobLifecycle(state, event);
      }
      // Not a queue event (logEvent, hardwareDetected, ...). Still advances the sequence
      // so a later event is not judged stale against a number this one never consumed.
      return { ...state, lastSequence: event.seq ?? state.lastSequence };
    }
  }
}

// --- selectors ------------------------------------------------------------------------------
// Views over the store. Kept here rather than in components so two components can never
// disagree about what "active" means.

export type QueueFilter = "ALL" | "ACTIVE" | "QUEUED" | "COMPLETED" | "FAILED" | "CANCELLED";

export type QueueSort = "QUEUE_ORDER" | "NEWEST" | "OLDEST" | "STATUS" | "TYPE";

const FILTER_STATES: Record<QueueFilter, ReadonlySet<JobState> | null> = {
  ALL: null,
  ACTIVE: new Set<JobState>(["STARTING", "RUNNING", "PAUSED", "RETRYING", "RETRY_WAIT"]),
  QUEUED: new Set<JobState>(["QUEUED", "WAITING"]),
  COMPLETED: new Set<JobState>(["COMPLETED"]),
  FAILED: new Set<JobState>(["FAILED", "SKIPPED"]),
  CANCELLED: new Set<JobState>(["CANCELLED"]),
};

export function filterJobs(jobs: JobSnapshot[], filter: QueueFilter): JobSnapshot[] {
  const allowed = FILTER_STATES[filter];
  return allowed === null ? jobs : jobs.filter((job) => allowed.has(job.state));
}

// Rank used by the STATUS sort: the order a user most likely wants to look at things in,
// not the enum's declaration order.
const STATUS_RANK: Record<JobState, number> = {
  RUNNING: 0,
  STARTING: 1,
  RETRYING: 2,
  PAUSED: 3,
  RETRY_WAIT: 4,
  QUEUED: 5,
  WAITING: 6,
  FAILED: 7,
  SKIPPED: 8,
  CANCELLED: 9,
  COMPLETED: 10,
};

export function sortJobs(
  jobs: JobSnapshot[],
  sort: QueueSort,
  pendingOrder: string[],
): JobSnapshot[] {
  const copy = [...jobs];
  const position = new Map(pendingOrder.map((id, index) => [id, index]));

  switch (sort) {
    case "QUEUE_ORDER":
      // The backend's order for pending work, then everything else by recency. This is the
      // only sort that reflects what the scheduler will actually do -- every other option
      // is a view, and the UI labels them as such (spec section 34).
      return copy.sort((a, b) => {
        const pa = position.get(a.id);
        const pb = position.get(b.id);
        if (pa !== undefined && pb !== undefined) return pa - pb;
        if (pa !== undefined) return -1;
        if (pb !== undefined) return 1;
        return b.createdAt.localeCompare(a.createdAt);
      });
    case "NEWEST":
      return copy.sort((a, b) => b.createdAt.localeCompare(a.createdAt));
    case "OLDEST":
      return copy.sort((a, b) => a.createdAt.localeCompare(b.createdAt));
    case "STATUS":
      return copy.sort(
        (a, b) => STATUS_RANK[a.state] - STATUS_RANK[b.state] || a.createdAt.localeCompare(b.createdAt),
      );
    case "TYPE":
      return copy.sort(
        (a, b) => a.type.localeCompare(b.type) || a.createdAt.localeCompare(b.createdAt),
      );
  }
}

export function allJobs(state: QueueState): JobSnapshot[] {
  return Object.values(state.jobs);
}
