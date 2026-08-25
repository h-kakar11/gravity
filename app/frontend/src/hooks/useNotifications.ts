import { useEffect } from "react";
import * as coreClient from "../services/coreClient";
import { notify } from "../services/notifications";
import type { CoreEvent, CoreEventData } from "../types/ipc";
import type { JobType } from "../types/job";

const JOB_TYPE_LABEL: Record<JobType, string> = {
  DOWNLOAD: "Download",
  CONVERSION: "Conversion",
  COMPRESSION: "Compression",
  BATCH: "Batch job",
  WORKFLOW: "Workflow",
  TEST: "Job",
};

async function notifyJobOutcome(event: CoreEvent): Promise<void> {
  if (event.event !== "jobCompleted" && event.event !== "jobFailed") return;
  if (!event.jobId) return;

  // Best-effort lookup for a nicer label ("Conversion complete" vs. plain "Job complete")
  // -- the job lifecycle events don't carry the type themselves, and a lookup failure
  // (e.g. the job already aged out of the live list) just falls back to the generic label.
  let label = "Job";
  try {
    const { job } = await coreClient.getJob(event.jobId);
    label = JOB_TYPE_LABEL[job.type];
  } catch {
    // Fall back silently.
  }

  if (event.event === "jobCompleted") {
    void notify(`${label} complete`, "Finished successfully.");
  } else {
    const data = event.data as CoreEventData["jobFailed"];
    void notify(`${label} failed`, data.error?.message ?? "The job failed.");
  }
}

function basename(path: string): string {
  const parts = path.split(/[\\/]/);
  return parts[parts.length - 1] || path;
}

// Fires OS toast notifications (Phase 4.5) for job completion/failure and for Watch
// Folders (4.1) / Scheduled Tasks (4.3) auto-submitting a job in the background.
// Subscribed once at the App level, not per-page, so a notification fires no matter which
// screen is currently showing -- mirrors App.tsx's global hotkey-event subscription.
export function useNotifications(): void {
  useEffect(() => {
    const unsubscribeJobs = coreClient.subscribeToJobEvents((event) => {
      void notifyJobOutcome(event);
    });
    const unsubscribeBackground = coreClient.subscribeToBackgroundEvents({
      onWatchFolderTriggered: (path) => {
        void notify("Watch folder", `Queued ${basename(path)} for processing.`);
      },
      onScheduledTaskFired: (taskName) => {
        void notify("Scheduled task", `"${taskName}" ran.`);
      },
    });

    return () => {
      unsubscribeJobs();
      unsubscribeBackground();
    };
  }, []);
}
