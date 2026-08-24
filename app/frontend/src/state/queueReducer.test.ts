// The frontend's half of event reconciliation (spec sections 46, 56, 57). The reducer is
// pure, so these are ordinary function calls -- no DOM, no React, no timers, and nothing
// that can flake.

import { describe, expect, it } from "vitest";

import {
  filterJobs,
  initialQueueState,
  queueReducer,
  sortJobs,
  type QueueState,
} from "./queueReducer";
import type { CoreEvent } from "../types/ipc";
import type { JobSnapshot, JobState } from "../types/job";
import type { QueueSnapshot } from "../types/queue";
import { EMPTY_STATISTICS } from "../types/queue";

function makeJob(id: string, overrides: Partial<JobSnapshot> = {}): JobSnapshot {
  return {
    id,
    type: "DOWNLOAD",
    state: "QUEUED",
    createdAt: "2026-01-01T00:00:00.000Z",
    progress: { statusMessage: "Queued" },
    priority: "NORMAL",
    attempt: 0,
    retryCount: 0,
    maxRetries: 3,
    dependencies: [],
    revision: 1,
    ...overrides,
  };
}

function makeSnapshot(jobs: JobSnapshot[], overrides: Partial<QueueSnapshot> = {}): QueueSnapshot {
  return {
    runState: "RUNNING",
    maxConcurrency: 2,
    statistics: { ...EMPTY_STATISTICS, total: jobs.length },
    jobs,
    pendingOrder: jobs.filter((j) => j.state === "QUEUED").map((j) => j.id),
    sequence: 10,
    ...overrides,
  };
}

function event<E extends CoreEvent["event"]>(
  name: E,
  seq: number,
  jobId: string | undefined,
  data: unknown,
): CoreEvent {
  return {
    event: name,
    seq,
    jobId,
    timestamp: "2026-01-01T00:00:01.000Z",
    data,
  } as CoreEvent;
}

function loaded(jobs: JobSnapshot[], overrides: Partial<QueueSnapshot> = {}): QueueState {
  return queueReducer(initialQueueState, {
    type: "snapshot",
    snapshot: makeSnapshot(jobs, overrides),
  });
}

describe("snapshot", () => {
  it("replaces the whole store and marks it loaded", () => {
    const state = loaded([makeJob("a"), makeJob("b", { state: "RUNNING" })]);

    expect(state.loaded).toBe(true);
    expect(Object.keys(state.jobs).sort()).toEqual(["a", "b"]);
    expect(state.lastSequence).toBe(10);
    expect(state.maxConcurrency).toBe(2);
  });

  it("drops jobs that are no longer in the snapshot", () => {
    const first = loaded([makeJob("a"), makeJob("b")]);
    const second = queueReducer(first, {
      type: "snapshot",
      snapshot: makeSnapshot([makeJob("a")], { sequence: 20 }),
    });

    expect(Object.keys(second.jobs)).toEqual(["a"]);
  });

  it("a reconnect snapshot resets the sequence baseline", () => {
    // A restarted core process starts numbering from 1 again. The snapshot's own sequence is
    // what the store trusts, so events from the new process are not judged stale against the
    // old process's much higher numbers.
    const state = loaded([makeJob("a")], { sequence: 5000 });
    const restarted = queueReducer(state, {
      type: "snapshot",
      snapshot: makeSnapshot([makeJob("a")], { sequence: 3 }),
    });

    expect(restarted.lastSequence).toBe(3);
    const applied = queueReducer(restarted, {
      type: "event",
      event: event("jobStarted", 4, "a", { state: "RUNNING", revision: 2 }),
    });
    expect(applied.jobs.a.state).toBe("RUNNING");
  });
});

describe("event ordering", () => {
  it("applies a normal lifecycle in order", () => {
    let state = loaded([makeJob("a")]);
    const steps: [string, number, JobState][] = [
      ["jobStarted", 11, "STARTING"],
      ["jobStarted", 12, "RUNNING"],
      ["jobCompleted", 13, "COMPLETED"],
    ];
    for (const [name, seq, jobState] of steps) {
      state = queueReducer(state, {
        type: "event",
        event: event(name as CoreEvent["event"], seq, "a", { state: jobState, revision: seq }),
      });
    }

    expect(state.jobs.a.state).toBe("COMPLETED");
    expect(state.lastSequence).toBe(13);
  });

  it("discards an event whose sequence is not newer", () => {
    const state = loaded([makeJob("a")]);
    const applied = queueReducer(state, {
      type: "event",
      event: event("jobCompleted", 13, "a", { state: "COMPLETED", revision: 5 }),
    });
    const stale = queueReducer(applied, {
      type: "event",
      event: event("jobStarted", 12, "a", { state: "RUNNING", revision: 4 }),
    });

    expect(stale.jobs.a.state).toBe("COMPLETED");
    expect(stale.droppedEvents).toBe(1);
  });

  it("discards a duplicate delivery of the same event", () => {
    const state = loaded([makeJob("a")]);
    const once = queueReducer(state, {
      type: "event",
      event: event("jobStarted", 11, "a", { state: "RUNNING", revision: 2 }),
    });
    const twice = queueReducer(once, {
      type: "event",
      event: event("jobStarted", 11, "a", { state: "RUNNING", revision: 2 }),
    });

    expect(twice.droppedEvents).toBe(1);
    expect(twice.jobs.a.state).toBe("RUNNING");
  });

  it("discards an event carrying an older job revision even when its sequence is newer", () => {
    // Two jobs' events interleave: a newer sequence number does not, by itself, mean newer
    // information about THIS job.
    const state = loaded([makeJob("a", { revision: 9, state: "COMPLETED" })]);
    const applied = queueReducer(state, {
      type: "event",
      event: event("jobStarted", 99, "a", { state: "RUNNING", revision: 4 }),
    });

    expect(applied.jobs.a.state).toBe("COMPLETED");
    expect(applied.droppedEvents).toBe(1);
  });

  it("advances the sequence for events it does not otherwise act on", () => {
    const state = loaded([makeJob("a")]);
    const applied = queueReducer(state, {
      type: "event",
      event: event("logEvent", 11, undefined, { level: "INFO", message: "hi", subsystem: "x" }),
    });

    expect(applied.lastSequence).toBe(11);
    // ...and a later real event is still accepted.
    const next = queueReducer(applied, {
      type: "event",
      event: event("jobStarted", 12, "a", { state: "RUNNING", revision: 2 }),
    });
    expect(next.jobs.a.state).toBe("RUNNING");
  });
});

describe("progress", () => {
  it("updates progress without touching state", () => {
    const state = loaded([makeJob("a", { state: "RUNNING" })]);
    const applied = queueReducer(state, {
      type: "event",
      event: event("jobProgress", 11, "a", {
        state: "RUNNING",
        percentage: 42,
        statusMessage: "Converting",
        speedBytesPerSecond: 1024,
      }),
    });

    expect(applied.jobs.a.progress.percentage).toBe(42);
    expect(applied.jobs.a.progress.statusMessage).toBe("Converting");
    expect(applied.jobs.a.state).toBe("RUNNING");
  });

  it("ignores progress for a job that has already finished", () => {
    // A progress event queued behind a completion event must not restart the progress bar.
    const state = loaded([makeJob("a", { state: "COMPLETED" })]);
    const applied = queueReducer(state, {
      type: "event",
      event: event("jobProgress", 11, "a", { state: "RUNNING", percentage: 50, statusMessage: "x" }),
    });

    expect(applied.jobs.a.progress.percentage).toBeUndefined();
    expect(applied.jobs.a.state).toBe("COMPLETED");
  });

  it("ignores progress for an unknown job", () => {
    const state = loaded([makeJob("a")]);
    const applied = queueReducer(state, {
      type: "event",
      event: event("jobProgress", 11, "ghost", { state: "RUNNING", percentage: 1, statusMessage: "x" }),
    });

    expect(applied.jobs.ghost).toBeUndefined();
    expect(applied.lastSequence).toBe(11);
  });
});

describe("job lifecycle events", () => {
  it("records the failure and its error", () => {
    const state = loaded([makeJob("a", { state: "RUNNING" })]);
    const applied = queueReducer(state, {
      type: "event",
      event: event("jobFailed", 11, "a", {
        state: "FAILED",
        revision: 4,
        error: {
          code: "E_FFMPEG_FAILED",
          category: "ENGINE_FAILURE",
          message: "boom",
          details: "",
          recoverable: false,
        },
      }),
    });

    expect(applied.jobs.a.state).toBe("FAILED");
    expect(applied.jobs.a.error?.code).toBe("E_FFMPEG_FAILED");
  });

  it("records a scheduled retry with its attempt and deadline", () => {
    const state = loaded([makeJob("a", { state: "FAILED" })]);
    const applied = queueReducer(state, {
      type: "event",
      event: event("jobRetryScheduled", 11, "a", {
        state: "RETRY_WAIT",
        revision: 5,
        attempt: 2,
        delayMs: 4000,
        reason: "network errors are usually temporary",
        maxRetries: 3,
        nextRetryAtMs: 1_700_000_000_000,
      }),
    });

    expect(applied.jobs.a.state).toBe("RETRY_WAIT");
    expect(applied.jobs.a.attempt).toBe(2);
    expect(applied.jobs.a.retryCount).toBe(2);
    expect(applied.jobs.a.nextRetryAtMs).toBe(1_700_000_000_000);
    expect(applied.jobs.a.retryReason).toContain("network");
  });

  it("records a skip caused by a failed dependency", () => {
    const state = loaded([makeJob("a", { state: "WAITING" })]);
    const applied = queueReducer(state, {
      type: "event",
      event: event("jobSkipped", 11, "a", {
        state: "SKIPPED",
        revision: 3,
        error: {
          code: "E_DEPENDENCY_FAILED",
          category: "UNKNOWN",
          message: "dependency failed",
          details: "",
          recoverable: false,
        },
      }),
    });

    expect(applied.jobs.a.state).toBe("SKIPPED");
    expect(applied.jobs.a.error?.code).toBe("E_DEPENDENCY_FAILED");
  });

  it("materialises a job it has never seen rather than dropping the event", () => {
    // This client connected mid-flight, or the job was created from another window.
    const state = loaded([]);
    const applied = queueReducer(state, {
      type: "event",
      event: event("jobCreated", 11, "new-job", { state: "QUEUED", revision: 1, type: "CONVERSION" }),
    });

    expect(applied.jobs["new-job"]).toBeDefined();
    expect(applied.jobs["new-job"].state).toBe("QUEUED");
  });
});

describe("queueChanged", () => {
  it("updates run state, concurrency, statistics and order together", () => {
    const state = loaded([makeJob("a"), makeJob("b")]);
    const applied = queueReducer(state, {
      type: "event",
      event: event("queueChanged", 11, undefined, {
        runState: "PAUSED",
        maxConcurrency: 4,
        statistics: { ...EMPTY_STATISTICS, queued: 2, total: 2 },
        pendingOrder: ["b", "a"],
      }),
    });

    expect(applied.runState).toBe("PAUSED");
    expect(applied.maxConcurrency).toBe(4);
    expect(applied.statistics.queued).toBe(2);
    expect(applied.pendingOrder).toEqual(["b", "a"]);
  });
});

describe("disconnected", () => {
  it("keeps the jobs but marks the store not loaded", () => {
    const state = loaded([makeJob("a")]);
    const applied = queueReducer(state, { type: "disconnected" });

    expect(applied.loaded).toBe(false);
    expect(applied.jobs.a).toBeDefined();
  });
});

describe("filters", () => {
  const jobs = [
    makeJob("running", { state: "RUNNING" }),
    makeJob("retrying", { state: "RETRY_WAIT" }),
    makeJob("queued", { state: "QUEUED" }),
    makeJob("waiting", { state: "WAITING" }),
    makeJob("done", { state: "COMPLETED" }),
    makeJob("failed", { state: "FAILED" }),
    makeJob("skipped", { state: "SKIPPED" }),
    makeJob("cancelled", { state: "CANCELLED" }),
  ];

  it("ALL shows everything", () => {
    expect(filterJobs(jobs, "ALL")).toHaveLength(jobs.length);
  });

  it("ACTIVE covers running and retrying work", () => {
    expect(filterJobs(jobs, "ACTIVE").map((j) => j.id).sort()).toEqual(["retrying", "running"]);
  });

  it("QUEUED covers both queued and dependency-blocked jobs", () => {
    expect(filterJobs(jobs, "QUEUED").map((j) => j.id).sort()).toEqual(["queued", "waiting"]);
  });

  it("FAILED includes skipped jobs, which the user also needs to see", () => {
    expect(filterJobs(jobs, "FAILED").map((j) => j.id).sort()).toEqual(["failed", "skipped"]);
  });

  it("CANCELLED and COMPLETED are exact", () => {
    expect(filterJobs(jobs, "CANCELLED").map((j) => j.id)).toEqual(["cancelled"]);
    expect(filterJobs(jobs, "COMPLETED").map((j) => j.id)).toEqual(["done"]);
  });
});

describe("sorting", () => {
  const jobs = [
    makeJob("c", { createdAt: "2026-01-03T00:00:00.000Z", state: "COMPLETED", type: "COMPRESSION" }),
    makeJob("a", { createdAt: "2026-01-01T00:00:00.000Z", state: "QUEUED", type: "DOWNLOAD" }),
    makeJob("b", { createdAt: "2026-01-02T00:00:00.000Z", state: "RUNNING", type: "CONVERSION" }),
  ];

  it("QUEUE_ORDER puts pending jobs first, in the backend's order", () => {
    const sorted = sortJobs(jobs, "QUEUE_ORDER", ["a"]);
    expect(sorted[0].id).toBe("a");
  });

  it("NEWEST and OLDEST are inverses", () => {
    expect(sortJobs(jobs, "NEWEST", []).map((j) => j.id)).toEqual(["c", "b", "a"]);
    expect(sortJobs(jobs, "OLDEST", []).map((j) => j.id)).toEqual(["a", "b", "c"]);
  });

  it("STATUS puts running work at the top and completed at the bottom", () => {
    const sorted = sortJobs(jobs, "STATUS", []);
    expect(sorted[0].state).toBe("RUNNING");
    expect(sorted[sorted.length - 1].state).toBe("COMPLETED");
  });

  it("TYPE groups by job type", () => {
    expect(sortJobs(jobs, "TYPE", []).map((j) => j.type)).toEqual([
      "COMPRESSION",
      "CONVERSION",
      "DOWNLOAD",
    ]);
  });

  it("does not mutate the array it was given", () => {
    const original = [...jobs];
    sortJobs(jobs, "NEWEST", []);
    expect(jobs).toEqual(original);
  });
});
