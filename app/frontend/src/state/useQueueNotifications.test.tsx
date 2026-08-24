// Restrained notifications (spec section 15): a toast for a real transition -- completed,
// failed, cancelled, retry scheduled -- and never for the progress events that fire many
// times a second, and never a flood of toasts for jobs that already existed when the app
// connected.

import { render, screen } from "@testing-library/react";
import { describe, expect, it } from "vitest";
import { useQueueNotifications } from "./useQueueNotifications";
import { ToastProvider } from "../components/ui/ToastProvider";
import { makeJob, makeQueueState } from "../test/fixtures";
import type { QueueState } from "./queueReducer";

function Harness({ state, enabled = true }: { state: QueueState; enabled?: boolean }) {
  useQueueNotifications(state, enabled);
  return null;
}

function renderWithState(initial: QueueState) {
  const utils = render(
    <ToastProvider>
      <Harness state={initial} />
    </ToastProvider>,
  );
  const rerenderWith = (next: QueueState) =>
    utils.rerender(
      <ToastProvider>
        <Harness state={next} />
      </ToastProvider>,
    );
  return { ...utils, rerenderWith };
}

describe("useQueueNotifications", () => {
  it("does not toast for the jobs already present in the first loaded snapshot", () => {
    const job = makeJob({ state: "COMPLETED" });
    render(
      <ToastProvider>
        <Harness state={makeQueueState({ jobs: { [job.id]: job } })} />
      </ToastProvider>,
    );
    expect(screen.queryByRole("status")).not.toBeInTheDocument();
  });

  it("toasts once a job actually transitions to COMPLETED after being seen running", () => {
    const running = makeJob({ state: "RUNNING" });
    const { rerenderWith } = renderWithState(makeQueueState({ jobs: { [running.id]: running } }));

    const completed = { ...running, state: "COMPLETED" as const, revision: running.revision + 1 };
    rerenderWith(makeQueueState({ jobs: { [completed.id]: completed } }));

    expect(screen.getByText("Job completed")).toBeInTheDocument();
  });

  it("toasts a failure and a cancellation as distinct outcomes", () => {
    const a = makeJob({ id: "a", state: "RUNNING" });
    const b = makeJob({ id: "b", state: "RUNNING" });
    const { rerenderWith } = renderWithState(makeQueueState({ jobs: { a, b } }));

    rerenderWith(
      makeQueueState({
        jobs: {
          a: { ...a, state: "FAILED", revision: 2 },
          b: { ...b, state: "CANCELLED", revision: 2 },
        },
      }),
    );

    expect(screen.getByText("Job failed")).toBeInTheDocument();
    expect(screen.getByText("Job cancelled")).toBeInTheDocument();
  });

  it("never toasts on a progress-only change, because progress never changes job.state", () => {
    const job = makeJob({ state: "RUNNING", progress: { statusMessage: "10%", percentage: 10 } });
    const { rerenderWith } = renderWithState(makeQueueState({ jobs: { [job.id]: job } }));

    rerenderWith(
      makeQueueState({ jobs: { [job.id]: { ...job, progress: { statusMessage: "55%", percentage: 55 } } } }),
    );

    expect(screen.queryByRole("status")).not.toBeInTheDocument();
  });

  it("says nothing at all when notifications are disabled", () => {
    const running = makeJob({ state: "RUNNING" });
    const { rerender } = render(
      <ToastProvider>
        <Harness state={makeQueueState({ jobs: { [running.id]: running } })} enabled={false} />
      </ToastProvider>,
    );
    rerender(
      <ToastProvider>
        <Harness
          state={makeQueueState({ jobs: { [running.id]: { ...running, state: "COMPLETED", revision: 2 } } })}
          enabled={false}
        />
      </ToastProvider>,
    );
    expect(screen.queryByRole("status")).not.toBeInTheDocument();
  });

  it("announces restart recovery once when the first snapshot contains an interrupted job", () => {
    const interrupted = makeJob({
      state: "FAILED",
      error: { code: "E_JOB_INTERRUPTED", category: "UNKNOWN", message: "x", details: "", recoverable: true },
    });
    render(
      <ToastProvider>
        <Harness state={makeQueueState({ jobs: { [interrupted.id]: interrupted } })} />
      </ToastProvider>,
    );
    expect(screen.getByText("Queue restored")).toBeInTheDocument();
  });
});
