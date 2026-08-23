import { useCallback, useEffect, useState } from "react";
import type { JobSnapshot } from "../types/job";
import type { CoreEvent, CoreEventName } from "../types/ipc";
import type { ErrorInfo } from "../types/error";
import * as coreClient from "../services/coreClient";

// Every event whose data carries at least `{state}` for a specific job (docs/ipc-contract.md
// "Events" section). Anything else (fileDetected, hardwareDetected, logEvent, ...) is not a
// job lifecycle event and this hook ignores it.
const JOB_LIFECYCLE_EVENTS: ReadonlySet<CoreEventName> = new Set([
  "jobCreated",
  "jobQueued",
  "jobStarted",
  "jobProgress",
  "jobPaused",
  "jobResumed",
  "jobCompleted",
  "jobFailed",
  "jobCancelled",
]);

function describeError(err: unknown): string {
  const info = err as Partial<ErrorInfo>;
  if (info && typeof info.message === "string") return info.message;
  return String(err);
}

// Keeps a live JobSnapshot[] in sync with the core process. Rather than hand-merging the
// partial `data` payload each lifecycle event carries, it re-fetches the full snapshot via
// getJob on every relevant event -- simpler and can't drift from what the core considers
// truth, at the cost of one extra round trip per event (acceptable for a Phase-1 dev console).
export function useJobs() {
  const [jobs, setJobs] = useState<JobSnapshot[]>([]);
  const [connectionError, setConnectionError] = useState<string | null>(null);

  const upsertJob = useCallback((job: JobSnapshot) => {
    setJobs((prev) => {
      const idx = prev.findIndex((j) => j.id === job.id);
      if (idx === -1) return [...prev, job];
      const next = [...prev];
      next[idx] = job;
      return next;
    });
  }, []);

  const refreshJob = useCallback(
    async (jobId: string) => {
      try {
        const { job } = await coreClient.getJob(jobId);
        upsertJob(job);
        setConnectionError(null);
      } catch (err) {
        setConnectionError(describeError(err));
      }
    },
    [upsertJob],
  );

  useEffect(() => {
    let active = true;

    coreClient
      .listJobs()
      .then(({ jobs: initial }) => {
        if (active) {
          setJobs(initial);
          setConnectionError(null);
        }
      })
      .catch((err) => {
        if (active) setConnectionError(describeError(err));
      });

    const unsubscribe = coreClient.subscribeToJobEvents((event: CoreEvent) => {
      if (event.jobId && JOB_LIFECYCLE_EVENTS.has(event.event)) {
        void refreshJob(event.jobId);
      }
    });

    return () => {
      active = false;
      unsubscribe();
    };
  }, [refreshJob]);

  const createTestJob = useCallback(async (): Promise<string> => {
    const { jobId } = await coreClient.createJob({ type: "TEST", params: {} });
    await refreshJob(jobId);
    return jobId;
  }, [refreshJob]);

  const cancelJob = useCallback(
    async (jobId: string): Promise<void> => {
      await coreClient.cancelJob(jobId);
      await refreshJob(jobId);
    },
    [refreshJob],
  );

  return { jobs, connectionError, createTestJob, cancelJob };
}
