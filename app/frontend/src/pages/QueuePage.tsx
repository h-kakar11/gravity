// The queue screen (spec sections 30-35). One list for every kind of job, with the controls
// that are valid for each, the queue-level controls, filters, statistics, and a detail panel.
//
// The backend is authoritative for everything shown here: this component renders the store
// and sends commands, and never edits a job locally. The sort selector is explicitly a view
// over that state -- picking "Newest" changes what you look at, not what the scheduler will
// do, and the UI says so.

import { useEffect, useMemo, useState } from "react";

import JobDetailPanel from "../components/JobDetailPanel";
import JobRow from "../components/JobRow";
import { styles } from "../components/queueStyles";
import {
  filterJobs,
  sortJobs,
  type QueueFilter,
  type QueueSort,
} from "../state/queueReducer";
import { useQueue } from "../state/useQueue";
import type { HistoryScope } from "../types/queue";
import { describeError } from "../utils/jobDisplay";

const FILTERS: { id: QueueFilter; label: string }[] = [
  { id: "ALL", label: "All" },
  { id: "ACTIVE", label: "Active" },
  { id: "QUEUED", label: "Queued" },
  { id: "COMPLETED", label: "Completed" },
  { id: "FAILED", label: "Failed" },
  { id: "CANCELLED", label: "Cancelled" },
];

const SORTS: { id: QueueSort; label: string }[] = [
  { id: "QUEUE_ORDER", label: "Queue order" },
  { id: "NEWEST", label: "Newest first" },
  { id: "OLDEST", label: "Oldest first" },
  { id: "STATUS", label: "Status" },
  { id: "TYPE", label: "Type" },
];

const CLEAR_SCOPES: { id: HistoryScope; label: string }[] = [
  { id: "COMPLETED", label: "Clear completed" },
  { id: "FAILED", label: "Clear failed" },
  { id: "CANCELLED", label: "Clear cancelled" },
  { id: "ALL", label: "Clear all finished" },
];

function Stat({ value, label }: { value: number; label: string }) {
  return (
    <div style={styles.stat}>
      <div style={styles.statValue}>{value}</div>
      <div style={styles.statLabel}>{label}</div>
    </div>
  );
}

export default function QueuePage() {
  const queue = useQueue();
  const { state } = queue;

  const [filter, setFilter] = useState<QueueFilter>("ALL");
  const [sort, setSort] = useState<QueueSort>("QUEUE_ORDER");
  const [selectedId, setSelectedId] = useState<string | null>(null);

  // Retry countdowns are the only thing on this screen that changes without an event, so a
  // one-second tick drives them rather than a general-purpose poll.
  const [nowMs, setNowMs] = useState(() => Date.now());
  useEffect(() => {
    const timer = window.setInterval(() => setNowMs(Date.now()), 1000);
    return () => window.clearInterval(timer);
  }, []);

  const visibleJobs = useMemo(
    () => sortJobs(filterJobs(queue.jobs, filter), sort, state.pendingOrder),
    [queue.jobs, filter, sort, state.pendingOrder],
  );

  const selectedJob = selectedId ? state.jobs[selectedId] : undefined;
  const stats = state.statistics;
  const paused = state.runState === "PAUSED";
  const hasFailures = stats.failed > 0;

  return (
    <div style={styles.page}>
      <h1 style={styles.h1}>Queue</h1>
      <p style={styles.subtitle}>
        Downloads, conversions and compressions all run through one queue. Pausing stops new
        jobs from starting; anything already running keeps going until it finishes or is
        cancelled.
      </p>

      {queue.actionError ? (
        <div style={styles.errorBanner} role="alert">
          <strong>{describeError(queue.actionError)}</strong>
          <button
            type="button"
            style={{ ...styles.button, marginLeft: "0.75rem" }}
            onClick={queue.clearActionError}
          >
            Dismiss
          </button>
        </div>
      ) : null}

      {!state.loaded ? (
        <div style={styles.errorBanner} role="status">
          Connecting to the core process…
          <button
            type="button"
            style={{ ...styles.button, marginLeft: "0.75rem" }}
            onClick={() => void queue.refresh()}
          >
            Retry
          </button>
        </div>
      ) : null}

      <div style={styles.statsRow} aria-label="Queue statistics">
        <Stat value={stats.running} label="Active" />
        <Stat value={stats.queued + stats.waiting} label="Queued" />
        <Stat value={stats.retryWait} label="Retrying" />
        <Stat value={stats.completed} label="Completed" />
        <Stat value={stats.failed} label="Failed" />
        <Stat value={stats.cancelled + stats.skipped} label="Stopped" />
      </div>

      <div style={styles.toolbar}>
        <div style={styles.toolbarGroup}>
          <button
            type="button"
            style={styles.buttonPrimary}
            onClick={() => void (paused ? queue.resumeQueue() : queue.pauseQueue())}
            aria-pressed={paused}
          >
            {paused ? "Resume queue" : "Pause queue"}
          </button>
          <span style={styles.toolbarLabel} role="status">
            {paused ? "Paused — no new jobs will start" : "Running"}
          </span>
        </div>

        <div style={styles.toolbarGroup}>
          <label style={styles.toolbarLabel} htmlFor="concurrency">
            Run at once
          </label>
          <select
            id="concurrency"
            value={state.maxConcurrency}
            onChange={(e) => void queue.setConcurrency(Number(e.target.value))}
            style={styles.button}
          >
            {[1, 2, 3, 4, 6, 8].map((value) => (
              <option key={value} value={value}>
                {value}
              </option>
            ))}
          </select>
        </div>

        <div style={styles.spacer} />

        <button
          type="button"
          style={hasFailures ? styles.button : { ...styles.button, ...styles.buttonDisabled }}
          disabled={!hasFailures}
          title={hasFailures ? "Retry every failed job" : "There are no failed jobs to retry"}
          onClick={() => void queue.retryFailed()}
        >
          Retry all failed
        </button>
        {CLEAR_SCOPES.map(({ id, label }) => (
          <button
            key={id}
            type="button"
            style={styles.button}
            title="Removes entries from this list. It never deletes files from disk."
            onClick={() => void queue.clearHistory(id)}
          >
            {label}
          </button>
        ))}
      </div>

      <div style={styles.filterBar} role="tablist" aria-label="Filter jobs">
        {FILTERS.map(({ id, label }) => (
          <button
            key={id}
            type="button"
            role="tab"
            aria-selected={filter === id}
            style={filter === id ? styles.filterTabActive : styles.filterTab}
            onClick={() => setFilter(id)}
          >
            {label}
          </button>
        ))}
        <div style={styles.spacer} />
        <label style={styles.toolbarLabel} htmlFor="sort">
          Sort
        </label>
        <select
          id="sort"
          value={sort}
          onChange={(e) => setSort(e.target.value as QueueSort)}
          style={styles.button}
        >
          {SORTS.map(({ id, label }) => (
            <option key={id} value={id}>
              {label}
            </option>
          ))}
        </select>
      </div>

      {sort !== "QUEUE_ORDER" ? (
        // Without this, re-sorting the list would look like it had re-prioritised the
        // queue. It has not (spec section 34).
        <p style={{ ...styles.subtitle, marginTop: 0 }} role="note">
          This is a display order only — the scheduler still runs jobs in queue order. Switch
          to “Queue order” to see what will actually run next.
        </p>
      ) : null}

      {visibleJobs.length === 0 ? (
        <div style={styles.empty}>
          {state.loaded
            ? filter === "ALL"
              ? "Nothing in the queue yet."
              : "No jobs match this filter."
            : "Loading…"}
        </div>
      ) : (
        <div style={styles.list}>
          {visibleJobs.map((job) => (
            <JobRow
              key={job.id}
              job={job}
              nowMs={nowMs}
              selected={selectedId === job.id}
              onSelect={(id) => setSelectedId((current) => (current === id ? null : id))}
              onCancel={(id) => void queue.cancelJob(id)}
              onRetry={(id) => void queue.retryJob(id)}
              onRemove={(id) => {
                if (selectedId === id) setSelectedId(null);
                void queue.removeJob(id);
              }}
              onMove={(id, direction) => void queue.moveJob(id, direction)}
            />
          ))}
        </div>
      )}

      {selectedJob ? (
        <JobDetailPanel
          job={selectedJob}
          nowMs={nowMs}
          onSetPriority={(id, priority) => void queue.setPriority(id, priority)}
          onClose={() => setSelectedId(null)}
        />
      ) : null}
    </div>
  );
}
