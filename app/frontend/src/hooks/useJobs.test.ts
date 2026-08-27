import { act, renderHook, waitFor } from "@testing-library/react";
import { beforeEach, describe, expect, it, vi } from "vitest";
import { useJobs } from "./useJobs";
import * as coreClient from "../services/coreClient";
import type { CoreEvent } from "../types/ipc";
import type { JobSnapshot } from "../types/job";

// useJobs.ts is the one piece of real event-merging logic this app has had since Phase 1
// -- these tests exercise it against a mocked coreClient rather than a real core process.
vi.mock("../services/coreClient", () => ({
  listJobs: vi.fn(),
  getJob: vi.fn(),
  subscribeToJobEvents: vi.fn(),
  createJob: vi.fn(),
  cancelJob: vi.fn(),
  pauseJob: vi.fn(),
  resumeJob: vi.fn(),
  retryJob: vi.fn(),
  removeJob: vi.fn(),
}));

function makeJob(overrides: Partial<JobSnapshot> = {}): JobSnapshot {
  return {
    id: "job-1",
    type: "CONVERSION",
    state: "RUNNING",
    createdAt: "2026-01-01T00:00:00.000Z",
    progress: { statusMessage: "Working" },
    ...overrides,
  };
}

describe("useJobs", () => {
  let eventCallback: ((event: CoreEvent) => void) | undefined;

  beforeEach(() => {
    vi.clearAllMocks();
    eventCallback = undefined;
    vi.mocked(coreClient.subscribeToJobEvents).mockImplementation((callback) => {
      eventCallback = callback;
      return () => {};
    });
    vi.mocked(coreClient.listJobs).mockResolvedValue({ jobs: [] });
  });

  it("loads jobs from listJobs on mount", async () => {
    vi.mocked(coreClient.listJobs).mockResolvedValue({ jobs: [makeJob()] });

    const { result } = renderHook(() => useJobs());

    await waitFor(() => expect(result.current.jobs).toHaveLength(1));
    expect(result.current.jobs[0].id).toBe("job-1");
    expect(result.current.connectionError).toBeNull();
  });

  it("re-fetches and upserts a job when a lifecycle event arrives for it", async () => {
    vi.mocked(coreClient.listJobs).mockResolvedValue({ jobs: [makeJob({ state: "RUNNING" })] });
    vi.mocked(coreClient.getJob).mockResolvedValue({ job: makeJob({ state: "COMPLETED" }) });

    const { result } = renderHook(() => useJobs());
    await waitFor(() => expect(result.current.jobs).toHaveLength(1));

    await act(async () => {
      eventCallback?.({
        event: "jobCompleted",
        jobId: "job-1",
        timestamp: "2026-01-01T00:00:01.000Z",
        data: { state: "COMPLETED" },
      });
      await Promise.resolve();
    });

    await waitFor(() => expect(result.current.jobs[0].state).toBe("COMPLETED"));
    expect(coreClient.getJob).toHaveBeenCalledWith("job-1");
  });

  it("ignores events with no jobId (e.g. hardwareDetected)", async () => {
    renderHook(() => useJobs());
    await waitFor(() => expect(coreClient.listJobs).toHaveBeenCalled());

    act(() => {
      eventCallback?.({
        event: "hardwareDetected",
        timestamp: "2026-01-01T00:00:01.000Z",
        data: { hardwareInfo: { cpu: { name: "x", logicalCores: 1 }, gpus: [], availableEncoders: [] } },
      });
    });

    expect(coreClient.getJob).not.toHaveBeenCalled();
  });

  it("sets connectionError when listJobs rejects", async () => {
    vi.mocked(coreClient.listJobs).mockRejectedValue(new Error("core unreachable"));

    const { result } = renderHook(() => useJobs());

    await waitFor(() => expect(result.current.connectionError).toBe("core unreachable"));
  });

  it("cancelJob calls coreClient.cancelJob then re-fetches the job", async () => {
    vi.mocked(coreClient.listJobs).mockResolvedValue({ jobs: [makeJob({ state: "RUNNING" })] });
    vi.mocked(coreClient.getJob).mockResolvedValue({ job: makeJob({ state: "CANCELLED" }) });
    vi.mocked(coreClient.cancelJob).mockResolvedValue({});

    const { result } = renderHook(() => useJobs());
    await waitFor(() => expect(result.current.jobs).toHaveLength(1));

    await act(async () => {
      await result.current.cancelJob("job-1");
    });

    expect(coreClient.cancelJob).toHaveBeenCalledWith("job-1");
    expect(result.current.jobs[0].state).toBe("CANCELLED");
  });

  // Issue #19: jobProgress used to trigger a full getJob() round-trip like every other
  // event, whose async responses could resolve out of order and visibly rewind the
  // progress bar. applyProgressEvent's whole point is applying the event's own payload
  // synchronously instead -- this asserts that path is actually taken (no getJob call).
  it("applies a jobProgress event directly without calling getJob", async () => {
    vi.mocked(coreClient.listJobs).mockResolvedValue({
      jobs: [makeJob({ state: "RUNNING", progress: { statusMessage: "Working", percentage: 10 } })],
    });

    const { result } = renderHook(() => useJobs());
    await waitFor(() => expect(result.current.jobs).toHaveLength(1));

    act(() => {
      eventCallback?.({
        event: "jobProgress",
        jobId: "job-1",
        timestamp: "2026-01-01T00:00:01.000Z",
        data: { state: "RUNNING", statusMessage: "Working", percentage: 55 },
      });
    });

    await waitFor(() => expect(result.current.jobs[0].progress.percentage).toBe(55));
    expect(result.current.jobs[0].state).toBe("RUNNING");
    expect(coreClient.getJob).not.toHaveBeenCalled();
  });

  // A progress event for a job not yet in local state can't be applied directly (it
  // doesn't carry a full JobSnapshot) -- must fall back to the normal getJob path.
  it("falls back to getJob for a jobProgress event on an unknown job", async () => {
    vi.mocked(coreClient.listJobs).mockResolvedValue({ jobs: [] });
    vi.mocked(coreClient.getJob).mockResolvedValue({
      job: makeJob({ id: "job-2", progress: { statusMessage: "Working", percentage: 20 } }),
    });

    renderHook(() => useJobs());
    await waitFor(() => expect(coreClient.listJobs).toHaveBeenCalled());

    act(() => {
      eventCallback?.({
        event: "jobProgress",
        jobId: "job-2",
        timestamp: "2026-01-01T00:00:01.000Z",
        data: { state: "RUNNING", statusMessage: "Working", percentage: 20 },
      });
    });

    await waitFor(() => expect(coreClient.getJob).toHaveBeenCalledWith("job-2"));
  });

  // Issue #29: removeJob deletes the job on the core side, so (unlike cancel/pause/
  // resume/retry) refetching via getJob afterwards would just fail -- it must drop the
  // job from local state directly instead.
  it("removeJob calls coreClient.removeJob and drops the job from local state", async () => {
    vi.mocked(coreClient.listJobs).mockResolvedValue({ jobs: [makeJob({ state: "COMPLETED" })] });
    vi.mocked(coreClient.removeJob).mockResolvedValue({});

    const { result } = renderHook(() => useJobs());
    await waitFor(() => expect(result.current.jobs).toHaveLength(1));

    await act(async () => {
      await result.current.removeJob("job-1");
    });

    expect(coreClient.removeJob).toHaveBeenCalledWith("job-1");
    expect(result.current.jobs).toHaveLength(0);
    expect(coreClient.getJob).not.toHaveBeenCalled();
  });
});
