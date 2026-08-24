// The queue screen (spec sections 9-11). One list for every kind of job, with the controls
// that are valid for each, the queue-level controls, filters, statistics, and a detail panel.
//
// The backend is authoritative for everything shown here: this component renders the store
// and sends commands, and never edits a job locally. The sort selector is explicitly a view
// over that state -- picking "Newest" changes what you look at, not what the scheduler will
// do, and the UI says so.

import { useEffect, useMemo, useState } from "react";

import JobDetailPanel from "../components/JobDetailPanel";
import JobRow from "../components/JobRow";
import {
  filterJobs,
  sortJobs,
  type QueueFilter,
  type QueueSort,
} from "../state/queueReducer";
import type { QueueController } from "../state/useQueue";
import type { HistoryScope } from "../types/queue";
import { describeError } from "../utils/jobDisplay";
import { AlertTriangleIcon, InboxIcon, PauseIcon, PlayIcon, RetryIcon, SearchIcon } from "../components/icons";
import { Button } from "../components/ui/Button";
import { EmptyState } from "../components/ui/EmptyState";
import { SkeletonRow } from "../components/ui/Skeleton";

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
    <div className="gv-stat">
      <div className="gv-stat__value">{value}</div>
      <div className="gv-stat__label">{label}</div>
    </div>
  );
}

export default function QueuePage({ queue }: { queue: QueueController }) {
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
    <div className="gv-enter">
      <h1 className="gv-h1">Queue</h1>
      <p className="gv-subtitle">
        Downloads, conversions and compressions all run through one queue. Pausing stops new
        jobs from starting; anything already running keeps going until it finishes or is
        cancelled.
      </p>

      {queue.actionError ? (
        <div className="gv-banner gv-banner--error" role="alert">
          <AlertTriangleIcon size={15} />
          <div className="gv-banner__body">
            <div className="gv-banner__title">{describeError(queue.actionError)}</div>
          </div>
          <Button variant="ghost" size="sm" onClick={queue.clearActionError}>
            Dismiss
          </Button>
        </div>
      ) : null}

      {!state.loaded ? (
        <div className="gv-banner gv-banner--warning" role="status">
          <AlertTriangleIcon size={15} />
          <div className="gv-banner__body">
            <div className="gv-banner__title">Connecting to the core process…</div>
          </div>
          <Button variant="ghost" size="sm" onClick={() => void queue.refresh()}>
            Retry
          </Button>
        </div>
      ) : null}

      <div className="gv-stats-row" aria-label="Queue statistics">
        <Stat value={stats.running} label="Active" />
        <Stat value={stats.queued + stats.waiting} label="Queued" />
        <Stat value={stats.retryWait} label="Retrying" />
        <Stat value={stats.completed} label="Completed" />
        <Stat value={stats.failed} label="Failed" />
        <Stat value={stats.cancelled + stats.skipped} label="Stopped" />
      </div>

      <div className="gv-toolbar">
        <div className="gv-toolbar__group">
          <Button
            variant={paused ? "primary" : "secondary"}
            icon={paused ? <PlayIcon size={14} /> : <PauseIcon size={14} />}
            onClick={() => void (paused ? queue.resumeQueue() : queue.pauseQueue())}
            aria-pressed={paused}
          >
            {paused ? "Resume queue" : "Pause queue"}
          </Button>
          <span className="gv-toolbar__label" role="status">
            {paused ? "Paused — no new jobs will start" : "Running"}
          </span>
        </div>

        <div className="gv-toolbar__group">
          <label className="gv-toolbar__label" htmlFor="concurrency">
            Run at once
          </label>
          <select
            id="concurrency"
            className="gv-select"
            style={{ width: "auto" }}
            value={state.maxConcurrency}
            onChange={(e) => void queue.setConcurrency(Number(e.target.value))}
          >
            {[1, 2, 3, 4, 6, 8].map((value) => (
              <option key={value} value={value}>
                {value}
              </option>
            ))}
          </select>
        </div>

        <div className="gv-spacer" />

        <Button
          variant="secondary"
          size="sm"
          icon={<RetryIcon size={13} />}
          disabled={!hasFailures}
          title={hasFailures ? "Retry every failed job" : "There are no failed jobs to retry"}
          onClick={() => void queue.retryFailed()}
        >
          Retry all failed
        </Button>
        {CLEAR_SCOPES.map(({ id, label }) => (
          <Button
            key={id}
            variant="ghost"
            size="sm"
            title="Removes entries from this list. It never deletes files from disk."
            onClick={() => void queue.clearHistory(id)}
          >
            {label}
          </Button>
        ))}
      </div>

      <div className="gv-tabs" role="tablist" aria-label="Filter jobs">
        {FILTERS.map(({ id, label }) => (
          <button
            key={id}
            type="button"
            role="tab"
            aria-selected={filter === id}
            className="gv-tab"
            onClick={() => setFilter(id)}
          >
            {label}
          </button>
        ))}
        <div className="gv-spacer" />
        <label className="gv-toolbar__label" htmlFor="sort">
          Sort
        </label>
        <select
          id="sort"
          className="gv-select"
          style={{ width: "auto" }}
          value={sort}
          onChange={(e) => setSort(e.target.value as QueueSort)}
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
        // queue. It has not (spec section 9).
        <p className="gv-hint" role="note" style={{ marginTop: "-0.5rem", marginBottom: "0.75rem" }}>
          This is a display order only — the scheduler still runs jobs in queue order. Switch
          to "Queue order" to see what will actually run next.
        </p>
      ) : null}

      {!state.loaded ? (
        <div className="gv-list">
          <SkeletonRow />
          <SkeletonRow />
          <SkeletonRow />
        </div>
      ) : visibleJobs.length === 0 ? (
        filter === "ALL" ? (
          <EmptyState
            icon={<InboxIcon size={28} />}
            title="Nothing in the queue yet"
            description="Start a download or a conversion and it will show up here."
          />
        ) : (
          <EmptyState
            icon={<SearchIcon size={26} />}
            title="No jobs match this filter"
            description="Try a different filter, or switch back to All."
            action={
              <Button variant="secondary" size="sm" onClick={() => setFilter("ALL")}>
                Show all jobs
              </Button>
            }
          />
        )
      ) : (
        <div className="gv-list">
          {visibleJobs.map((job) => (
            <JobRow
              key={job.id}
              job={job}
              // Only a RETRY_WAIT row's countdown depends on the clock, so every other row
              // gets a constant here -- an unchanging prop across the once-a-second tick,
              // which is what lets JobRow's memoization actually skip re-rendering them
              // (spec section 20).
              nowMs={job.state === "RETRY_WAIT" ? nowMs : 0}
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
