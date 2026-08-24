// User-visible queue states (spec sections 10, 23): loading, empty, filtered-empty, and a
// populated list with controls gated correctly by job state. Deliberately not a happy-path
// only test -- these are exactly the states spec section 10 asks to be designed for.

import { render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it } from "vitest";
import QueuePage from "./QueuePage";
import { makeJob, makeQueueController, makeQueueState } from "../test/fixtures";

describe("QueuePage", () => {
  it("shows a connecting state before the first snapshot loads", () => {
    const queue = makeQueueController({ state: makeQueueState({ loaded: false }) });
    render(<QueuePage queue={queue} />);
    expect(screen.getByText(/connecting to the core process/i)).toBeInTheDocument();
  });

  it("shows an empty state with no next-action pressure when the queue truly has nothing", () => {
    const queue = makeQueueController();
    render(<QueuePage queue={queue} />);
    expect(screen.getByText(/nothing in the queue yet/i)).toBeInTheDocument();
  });

  it("shows a distinct empty state, with a way back, when a filter hides everything", async () => {
    const job = makeJob({ state: "COMPLETED" });
    const state = makeQueueState({ jobs: { [job.id]: job } });
    const queue = makeQueueController({ state, jobs: [job] });
    const user = userEvent.setup();
    render(<QueuePage queue={queue} />);

    await user.click(screen.getByRole("tab", { name: "Failed" }));
    expect(screen.getByText(/no jobs match this filter/i)).toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: /show all jobs/i }));
    expect(screen.getByText(job.id, { exact: false })).toBeInTheDocument();
  });

  it("surfaces the backend's action error with a way to dismiss it", async () => {
    const queue = makeQueueController({
      actionError: { code: "E_JOB_NOT_FOUND", category: "UNKNOWN", message: "gone", details: "", recoverable: false },
    });
    const user = userEvent.setup();
    render(<QueuePage queue={queue} />);
    expect(screen.getByRole("alert")).toBeInTheDocument();
    await user.click(screen.getByRole("button", { name: "Dismiss" }));
    expect(queue.clearActionError).toHaveBeenCalledTimes(1);
  });

  it("sends cancelJob when a running job's Cancel control is used", async () => {
    const job = makeJob({ state: "RUNNING", progress: { statusMessage: "Downloading" } });
    const state = makeQueueState({ jobs: { [job.id]: job } });
    const queue = makeQueueController({ state, jobs: [job] });
    const user = userEvent.setup();
    render(<QueuePage queue={queue} />);

    await user.click(screen.getByRole("button", { name: `Cancel ${job.id}` }));
    expect(queue.cancelJob).toHaveBeenCalledWith(job.id);
  });

  it("disables Retry for a job that has not failed", () => {
    const job = makeJob({ state: "RUNNING" });
    const state = makeQueueState({ jobs: { [job.id]: job } });
    const queue = makeQueueController({ state, jobs: [job] });
    render(<QueuePage queue={queue} />);
    expect(screen.getByRole("button", { name: `Retry ${job.id}` })).toBeDisabled();
  });

  it("pauses the queue via the toolbar control", async () => {
    const queue = makeQueueController();
    const user = userEvent.setup();
    render(<QueuePage queue={queue} />);
    await user.click(screen.getByRole("button", { name: /pause queue/i }));
    expect(queue.pauseQueue).toHaveBeenCalledTimes(1);
  });
});
