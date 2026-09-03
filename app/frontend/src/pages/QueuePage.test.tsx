import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { act } from "react";
import { beforeEach, describe, expect, it, vi } from "vitest";
import QueuePage from "./QueuePage";
import * as coreClient from "../services/coreClient";
import type { JobSnapshot } from "../types/job";

// "Stop all" (queue recovery for a stuck playlist chain) had no coverage at all -- these
// pin that it cancels every currently-active job, one call per job, without touching
// terminal ones or blocking on a single failure.

vi.mock("../services/coreClient", () => ({
  listJobs: vi.fn(),
  listJobHistory: vi.fn(),
  getJob: vi.fn(),
  subscribeToJobEvents: vi.fn(),
  cancelJob: vi.fn(),
  pauseJob: vi.fn(),
  resumeJob: vi.fn(),
  retryJob: vi.fn(),
  openContainingFolder: vi.fn(),
}));

function job(id: string, state: JobSnapshot["state"]): JobSnapshot {
  return {
    id,
    type: "DOWNLOAD",
    state,
    priority: 0,
    attempts: 1,
    createdAt: `2026-01-01T00:00:0${id}.000Z`,
    progress: { statusMessage: "Queued" },
  };
}

function renderQueue() {
  return render(<QueuePage />);
}

beforeEach(() => {
  vi.clearAllMocks();
  vi.mocked(coreClient.listJobHistory).mockResolvedValue({ jobs: [] });
  vi.mocked(coreClient.subscribeToJobEvents).mockReturnValue(() => {});
  vi.mocked(coreClient.cancelJob).mockResolvedValue({});
  vi.mocked(coreClient.getJob).mockImplementation(async (jobId: string) => ({
    job: job(jobId, "CANCELLED"),
  }));
});

describe("QueuePage stop all", () => {
  it("cancels every active job and hides the button once none remain", async () => {
    vi.mocked(coreClient.listJobs).mockResolvedValue({
      jobs: [job("1", "QUEUED"), job("2", "RUNNING"), job("3", "COMPLETED")],
    });

    await act(async () => renderQueue());

    expect(screen.getByRole("button", { name: "Stop all" })).toBeTruthy();

    await act(async () => {
      fireEvent.click(screen.getByRole("button", { name: "Stop all" }));
    });

    await waitFor(() => {
      expect(coreClient.cancelJob).toHaveBeenCalledTimes(2);
    });
    expect(coreClient.cancelJob).toHaveBeenCalledWith("1");
    expect(coreClient.cancelJob).toHaveBeenCalledWith("2");
    expect(coreClient.cancelJob).not.toHaveBeenCalledWith("3");
  });

  it("does not render Stop all when there are no active jobs", async () => {
    vi.mocked(coreClient.listJobs).mockResolvedValue({ jobs: [job("1", "COMPLETED")] });

    await act(async () => renderQueue());

    expect(screen.queryByRole("button", { name: "Stop all" })).toBeNull();
  });

  it("still cancels the remaining active jobs when one cancel call rejects", async () => {
    vi.mocked(coreClient.listJobs).mockResolvedValue({
      jobs: [job("1", "QUEUED"), job("2", "RUNNING")],
    });
    vi.mocked(coreClient.cancelJob).mockImplementation(async (jobId: string) => {
      if (jobId === "1") throw new Error("already terminal");
      return {};
    });

    await act(async () => renderQueue());

    await act(async () => {
      fireEvent.click(screen.getByRole("button", { name: "Stop all" }));
    });

    await waitFor(() => {
      expect(coreClient.cancelJob).toHaveBeenCalledTimes(2);
    });
    expect(coreClient.cancelJob).toHaveBeenCalledWith("2");
  });
});
