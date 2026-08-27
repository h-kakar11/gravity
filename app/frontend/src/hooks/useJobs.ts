import { useCallback, useEffect, useRef, useState } from "react";
import type { JobSnapshot } from "../types/job";
import type { CoreEvent, CoreEventData, CoreEventName } from "../types/ipc";
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

// Keeps a live JobSnapshot[] in sync with the core process. Most lifecycle events still
// re-fetch the full snapshot via getJob -- simpler and can't drift from what the core
// considers truth. jobProgress is the one deliberate exception (see applyProgressEvent
// below): it fires many times a second, and getJob's async round trips can resolve out of
// order, which used to visibly rewind the progress bar (issue #19).
export function useJobs() {
  const [jobs, setJobs] = useState<JobSnapshot[]>([]);
  const [connectionError, setConnectionError] = useState<string | null>(null);

  // Mirrors `jobs` for applyProgressEvent's synchronous membership check below. A
  // React state setter's functional-updater form (setJobs(prev => ...)) does not run
  // synchronously -- it's queued for the next render -- so a flag set inside that updater
  // and read immediately after calling setJobs is not reliable (it was almost always
  // still false, silently defeating the whole point of issue #19's fix: every progress
  // event fell through to the getJob round-trip it was meant to avoid). A ref mutated in
  // an effect is synchronously current by the time this callback can run.
  const jobsRef = useRef<JobSnapshot[]>([]);
  useEffect(() => {
    jobsRef.current = jobs;
  }, [jobs]);

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

  // Applies a jobProgress event's payload directly onto the matching job already in
  // state, synchronously -- no IPC round trip, so there's nothing async to arrive out of
  // order and rewind the bar. Returns false (caller should fall back to refreshJob) if the
  // job isn't in local state yet, since the event doesn't carry a full JobSnapshot.
  const applyProgressEvent = useCallback((jobId: string, data: CoreEventData["jobProgress"]): boolean => {
    if (!jobsRef.current.some((j) => j.id === jobId)) return false;
    const { state, ...progress } = data;
    setJobs((prev) => {
      const idx = prev.findIndex((j) => j.id === jobId);
      if (idx === -1) return prev;
      const next = [...prev];
      next[idx] = { ...next[idx], state, progress };
      return next;
    });
    return true;
  }, []);

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
      if (!event.jobId || !JOB_LIFECYCLE_EVENTS.has(event.event)) return;
      if (event.event === "jobProgress" && applyProgressEvent(event.jobId, event.data as CoreEventData["jobProgress"])) {
        return;
      }
      void refreshJob(event.jobId);
    });

    return () => {
      active = false;
      unsubscribe();
    };
  }, [refreshJob, applyProgressEvent]);

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

  const pauseJob = useCallback(
    async (jobId: string): Promise<void> => {
      await coreClient.pauseJob(jobId);
      await refreshJob(jobId);
    },
    [refreshJob],
  );

  const resumeJob = useCallback(
    async (jobId: string): Promise<void> => {
      await coreClient.resumeJob(jobId);
      await refreshJob(jobId);
    },
    [refreshJob],
  );

  const retryJob = useCallback(
    async (jobId: string): Promise<void> => {
      await coreClient.retryJob(jobId);
      await refreshJob(jobId);
    },
    [refreshJob],
  );

  // Unlike cancel/pause/resume/retry, a removed job no longer exists on the core side --
  // refreshJob's getJob would just fail. Drop it from local state directly instead.
  // Powers the Queue screen's "clear completed" (issue #29).
  const removeJob = useCallback(async (jobId: string): Promise<void> => {
    await coreClient.removeJob(jobId);
    setJobs((prev) => prev.filter((j) => j.id !== jobId));
  }, []);

  return { jobs, connectionError, createTestJob, cancelJob, pauseJob, resumeJob, retryJob, removeJob };
}
