// Bridges the queue store to toast notifications (spec section 15). Fires for outcomes worth
// knowing about away from the Queue screen -- completed, failed, cancelled, an automatic
// retry being scheduled -- and for the one-time "queue restored after restart" signal. Never
// for progress: this hook only looks at state transitions, and jobProgress events never
// change `state`, so they can't reach it.
//
// Deliberately outside queueReducer.ts: the reducer stays a pure function of (state, event),
// and toast side effects live here, driven by diffing the store's own output. That keeps
// "what the store contains" and "what the user is told about it" as two separate, each
// independently testable, concerns.

import { useEffect, useRef } from "react";
import type { JobSnapshot, JobState } from "../types/job";
import { jobTitle } from "../utils/jobDisplay";
import { useToasts } from "../components/ui/ToastProvider";
import type { QueueState } from "./queueReducer";

const NOTIFIABLE: ReadonlySet<JobState> = new Set(["COMPLETED", "FAILED", "CANCELLED", "RETRY_WAIT"]);

export function useQueueNotifications(state: QueueState, notificationsEnabled = true): void {
  const { push } = useToasts();
  const prevJobs = useRef<Record<string, JobSnapshot> | null>(null);
  const announcedRestart = useRef(false);

  useEffect(() => {
    if (!state.loaded) return;
    if (!notificationsEnabled) {
      prevJobs.current = state.jobs;
      return;
    }

    const prev = prevJobs.current;

    // First loaded snapshot: look once for restart-recovery evidence (spec section 15,
    // "queue restored after restart") rather than toasting for every pre-existing job.
    if (prev === null) {
      const interrupted = Object.values(state.jobs).filter(
        (job) => job.error?.code === "E_JOB_INTERRUPTED",
      );
      if (interrupted.length > 0 && !announcedRestart.current) {
        announcedRestart.current = true;
        push({
          tone: "info",
          title: "Queue restored",
          detail:
            interrupted.length === 1
              ? "1 job was interrupted when Gravity last closed and needs a retry."
              : `${interrupted.length} jobs were interrupted when Gravity last closed and need a retry.`,
        });
      }
      prevJobs.current = state.jobs;
      return;
    }

    for (const job of Object.values(state.jobs)) {
      const before = prev[job.id];
      if (!before || before.state === job.state) continue;
      if (!NOTIFIABLE.has(job.state)) continue;

      const title = jobTitle(job);
      switch (job.state) {
        case "COMPLETED":
          push({ tone: "success", title: "Job completed", detail: title });
          break;
        case "FAILED":
          push({ tone: "error", title: "Job failed", detail: title });
          break;
        case "CANCELLED":
          push({ tone: "info", title: "Job cancelled", detail: title });
          break;
        case "RETRY_WAIT":
          push({ tone: "info", title: "Retry scheduled", detail: title });
          break;
        default:
          break;
      }
    }

    prevJobs.current = state.jobs;
  }, [state.loaded, state.jobs, notificationsEnabled, push]);
}
