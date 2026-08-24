// Presentation rules that are easy to get quietly wrong: which controls a job's state
// allows, and how backend facts turn into words (spec sections 31, 36, 37).

import { describe, expect, it } from "vitest";

import {
  canCancel,
  canChangePriority,
  canRemove,
  canReorder,
  canRetry,
  describeError,
  formatBytes,
  formatDuration,
  formatSpeed,
  jobSubtitle,
  jobTitle,
  secondsUntilRetry,
  truncateMiddle,
} from "./jobDisplay";
import type { JobSnapshot, JobState } from "../types/job";

function makeJob(overrides: Partial<JobSnapshot> = {}): JobSnapshot {
  return {
    id: "job-1",
    type: "DOWNLOAD",
    state: "QUEUED",
    createdAt: "2026-01-01T00:00:00.000Z",
    progress: { statusMessage: "" },
    priority: "NORMAL",
    attempt: 0,
    retryCount: 0,
    maxRetries: 3,
    dependencies: [],
    revision: 1,
    ...overrides,
  };
}

describe("formatting", () => {
  it("scales byte units", () => {
    expect(formatBytes(512)).toBe("512 B");
    expect(formatBytes(2048)).toBe("2 KB");
    expect(formatBytes(5 * 1024 * 1024)).toBe("5.0 MB");
    expect(formatBytes(3 * 1024 * 1024 * 1024)).toBe("3.00 GB");
  });

  it("reports missing or nonsensical values as unknown rather than zero", () => {
    expect(formatBytes(undefined)).toBe("--");
    expect(formatSpeed(undefined)).toBe("--");
    expect(formatDuration(undefined)).toBe("--");
    expect(formatDuration(Infinity)).toBe("--");
    expect(formatDuration(-5)).toBe("--");
  });

  it("formats durations with and without hours", () => {
    expect(formatDuration(65)).toBe("1:05");
    expect(formatDuration(3661)).toBe("1:01:01");
  });

  it("truncates long strings in the middle, keeping both ends", () => {
    const long = "a".repeat(40) + "DISTINCTIVE" + "b".repeat(40);
    const short = truncateMiddle(long, 21);
    expect(short.length).toBeLessThanOrEqual(21);
    expect(short.startsWith("aaaa")).toBe(true);
    expect(short.endsWith("bbbb")).toBe(true);
    expect(short).toContain("…");
  });

  it("leaves a short string alone", () => {
    expect(truncateMiddle("short.mp4", 64)).toBe("short.mp4");
  });
});

describe("error presentation", () => {
  it("maps a known code to a sentence, not the raw code", () => {
    const text = describeError({
      code: "E_INPUT_NOT_FOUND",
      category: "FILE_NOT_FOUND",
      message: "The input file no longer exists.",
      details: "path=/x",
      recoverable: false,
    });
    expect(text).toContain("could not be found");
    expect(text).not.toContain("E_INPUT_NOT_FOUND");
  });

  it("falls back to the backend's own message for an unmapped code", () => {
    expect(
      describeError({
        code: "E_SOMETHING_NEW",
        category: "UNKNOWN",
        message: "Backend explained it fine",
        details: "",
        recoverable: false,
      }),
    ).toBe("Backend explained it fine");
  });

  it("never renders undefined for a job with no error", () => {
    expect(describeError(undefined)).toBe("");
  });
});

describe("control availability", () => {
  const cases: { state: JobState; cancel: boolean; retry: boolean; remove: boolean }[] = [
    { state: "QUEUED", cancel: true, retry: false, remove: false },
    { state: "WAITING", cancel: true, retry: false, remove: false },
    { state: "STARTING", cancel: true, retry: false, remove: false },
    { state: "RUNNING", cancel: true, retry: false, remove: false },
    { state: "PAUSED", cancel: true, retry: false, remove: false },
    { state: "RETRY_WAIT", cancel: true, retry: true, remove: false },
    { state: "RETRYING", cancel: true, retry: false, remove: false },
    { state: "COMPLETED", cancel: false, retry: false, remove: true },
    { state: "FAILED", cancel: false, retry: true, remove: true },
    { state: "CANCELLED", cancel: false, retry: false, remove: true },
    { state: "SKIPPED", cancel: false, retry: true, remove: true },
  ];

  it.each(cases)("$state allows the right controls", ({ state, cancel, retry, remove }) => {
    const job = makeJob({ state });
    expect(canCancel(job)).toBe(cancel);
    expect(canRetry(job)).toBe(retry);
    expect(canRemove(job)).toBe(remove);
  });

  it("cancel and remove are never both available", () => {
    // A job is either still stoppable or already finished -- never both, which would let
    // the UI offer two contradictory actions.
    for (const { state } of cases) {
      const job = makeJob({ state });
      expect(canCancel(job) && canRemove(job)).toBe(false);
    }
  });

  it("only a pending job can be reordered", () => {
    expect(canReorder(makeJob({ state: "QUEUED", queuePosition: 0 }))).toBe(true);
    expect(canReorder(makeJob({ state: "RUNNING" }))).toBe(false);
    expect(canReorder(makeJob({ state: "COMPLETED" }))).toBe(false);
  });

  it("a finished job's priority cannot be changed", () => {
    expect(canChangePriority(makeJob({ state: "QUEUED" }))).toBe(true);
    expect(canChangePriority(makeJob({ state: "RUNNING" }))).toBe(true);
    expect(canChangePriority(makeJob({ state: "COMPLETED" }))).toBe(false);
    expect(canChangePriority(makeJob({ state: "FAILED" }))).toBe(false);
  });
});

describe("retry countdown", () => {
  it("counts down to the scheduled instant", () => {
    const job = makeJob({ state: "RETRY_WAIT", nextRetryAtMs: 10_000 });
    expect(secondsUntilRetry(job, 4_000)).toBe(6);
  });

  it("never goes negative once the deadline has passed", () => {
    const job = makeJob({ state: "RETRY_WAIT", nextRetryAtMs: 10_000 });
    expect(secondsUntilRetry(job, 99_000)).toBe(0);
  });

  it("is undefined for a job that is not waiting to retry", () => {
    expect(secondsUntilRetry(makeJob({ state: "RUNNING" }), 0)).toBeUndefined();
    expect(secondsUntilRetry(makeJob({ state: "RETRY_WAIT" }), 0)).toBeUndefined();
  });
});

describe("job description", () => {
  it("prefers a download's title over its id", () => {
    expect(jobTitle(makeJob({ metadata: { title: "A Video" } }))).toBe("A Video");
  });

  it("falls back to the input filename for a local job", () => {
    expect(
      jobTitle(makeJob({ type: "CONVERSION", metadata: { inputFilename: "clip.mp4" } })),
    ).toBe("clip.mp4");
  });

  it("falls back to the id only when nothing else is known", () => {
    expect(jobTitle(makeJob())).toBe("job-1");
  });

  it("describes a conversion in terms of its formats", () => {
    const subtitle = jobSubtitle(
      makeJob({
        type: "CONVERSION",
        metadata: { sourceFormat: "mp4", targetFormat: "mp3", outputFilename: "clip.mp3" },
      }),
    );
    expect(subtitle).toContain("MP4 → MP3");
    expect(subtitle).toContain("clip.mp3");
  });

  it("never exposes an ffmpeg command line", () => {
    const subtitle = jobSubtitle(
      makeJob({ type: "COMPRESSION", metadata: { preset: "LOW", maxHeight: 720 } }),
    );
    expect(subtitle.toLowerCase()).not.toContain("ffmpeg");
    expect(subtitle).not.toContain("-crf");
  });
});
