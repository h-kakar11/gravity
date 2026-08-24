// React binding for the queue store. All the interesting logic lives in queueReducer.ts;
// this file is only the wiring: fetch the initial snapshot, subscribe to events, and expose
// commands that go to the backend rather than mutating local state.
//
// The backend stays authoritative throughout: every action here sends a command and waits
// for the resulting events to update the store. Nothing optimistically edits a job.

import { useCallback, useEffect, useMemo, useReducer, useRef, useState } from "react";

import * as coreClient from "../services/coreClient";
import { asErrorInfo } from "../utils/errors";
import type { ErrorInfo } from "../types/error";
import type { JobPriority } from "../types/job";
import type { HistoryScope, MoveDirection } from "../types/queue";
import {
  allJobs,
  initialQueueState,
  queueReducer,
  type QueueState,
} from "./queueReducer";

export interface QueueController {
  state: QueueState;
  jobs: ReturnType<typeof allJobs>;
  // Last error raised by a command the user triggered, for display next to the controls.
  actionError: ErrorInfo | null;
  clearActionError: () => void;
  refresh: () => Promise<void>;

  cancelJob: (jobId: string) => Promise<void>;
  retryJob: (jobId: string) => Promise<void>;
  removeJob: (jobId: string) => Promise<void>;
  setPriority: (jobId: string, priority: JobPriority) => Promise<void>;
  moveJob: (jobId: string, direction: MoveDirection) => Promise<void>;

  pauseQueue: () => Promise<void>;
  resumeQueue: () => Promise<void>;
  setConcurrency: (value: number) => Promise<void>;
  clearHistory: (scope: HistoryScope) => Promise<void>;
  retryFailed: () => Promise<void>;
}

export function useQueue(): QueueController {
  const [state, dispatch] = useReducer(queueReducer, initialQueueState);
  const [actionError, setActionError] = useState<ErrorInfo | null>(null);
  const mounted = useRef(true);

  const refresh = useCallback(async () => {
    try {
      const { queue } = await coreClient.getQueueSnapshot();
      if (mounted.current) dispatch({ type: "snapshot", snapshot: queue });
    } catch (err) {
      if (mounted.current) {
        dispatch({ type: "disconnected" });
        setActionError(asErrorInfo(err));
      }
    }
  }, []);

  useEffect(() => {
    mounted.current = true;

    // Subscribe BEFORE fetching the snapshot. Events that arrive during the fetch are then
    // applied on top of it and, because the snapshot carries the sequence number it was
    // taken at, anything already reflected in it is discarded rather than double-applied.
    const unsubscribe = coreClient.subscribeToJobEvents((event) => {
      if (mounted.current) dispatch({ type: "event", event });
    });
    void refresh();

    return () => {
      mounted.current = false;
      unsubscribe();
    };
  }, [refresh]);

  // Wraps a command so a rejection becomes a displayable ErrorInfo instead of an unhandled
  // promise rejection. Commands legitimately fail (retrying a running job, moving a job
  // that just started) and the UI has to say so rather than swallow it.
  const run = useCallback(async (action: () => Promise<unknown>) => {
    try {
      setActionError(null);
      await action();
    } catch (err) {
      if (mounted.current) setActionError(asErrorInfo(err));
    }
  }, []);

  const jobs = useMemo(() => allJobs(state), [state]);

  return {
    state,
    jobs,
    actionError,
    clearActionError: useCallback(() => setActionError(null), []),
    refresh,

    cancelJob: useCallback((jobId) => run(() => coreClient.cancelJob(jobId)), [run]),
    retryJob: useCallback((jobId) => run(() => coreClient.retryJob(jobId)), [run]),
    removeJob: useCallback((jobId) => run(() => coreClient.removeJob(jobId)), [run]),
    setPriority: useCallback(
      (jobId, priority) => run(() => coreClient.setJobPriority(jobId, priority)),
      [run],
    ),
    moveJob: useCallback(
      (jobId, direction) => run(() => coreClient.moveJob(jobId, direction)),
      [run],
    ),

    pauseQueue: useCallback(() => run(() => coreClient.pauseQueue()), [run]),
    resumeQueue: useCallback(() => run(() => coreClient.resumeQueue()), [run]),
    setConcurrency: useCallback(
      (value) => run(() => coreClient.setConcurrency(value)),
      [run],
    ),
    clearHistory: useCallback((scope) => run(() => coreClient.clearHistory(scope)), [run]),
    retryFailed: useCallback(() => run(() => coreClient.retryFailedJobs()), [run]),
  };
}
