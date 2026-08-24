// One job in the queue list. Renders the same shape for every job type -- the differences
// live in jobDisplay's title/subtitle helpers, not in three parallel components.
//
// Wrapped in React.memo (spec section 20): the queue can produce a jobProgress event several
// times a second for whichever job is running, which re-renders that one row through
// useQueue's state update. Memoizing means the OTHER rows -- queued, completed, failed, the
// majority of the list once there are a few dozen jobs -- skip re-rendering entirely because
// their own JobSnapshot object identity hasn't changed.

import { memo } from "react";

import type { JobSnapshot } from "../types/job";
import type { MoveDirection } from "../types/queue";
import {
  canCancel,
  canRemove,
  canReorder,
  canRetry,
  describeError,
  formatBytes,
  formatDuration,
  formatSpeed,
  jobSubtitle,
  jobTitle,
  JOB_TYPE_LABELS,
  PRIORITY_LABELS,
  secondsUntilRetry,
} from "../utils/jobDisplay";
import { StatusBadge } from "./ui/StatusBadge";
import { CancelIcon, ChevronDownIcon, ChevronToTopIcon, ChevronUpIcon, RetryIcon, TrashIcon } from "./icons";

interface JobRowProps {
  job: JobSnapshot;
  selected: boolean;
  nowMs: number;
  onSelect: (jobId: string) => void;
  onCancel: (jobId: string) => void;
  onRetry: (jobId: string) => void;
  onRemove: (jobId: string) => void;
  onMove: (jobId: string, direction: MoveDirection) => void;
}

// A button that is present but unavailable, rather than absent. `title` doubles as the
// accessible explanation of why it is disabled.
function ControlButton({
  icon,
  ariaLabel,
  title,
  enabled,
  danger,
  onClick,
}: {
  icon: React.ReactNode;
  ariaLabel: string;
  title: string;
  enabled: boolean;
  danger?: boolean;
  onClick: () => void;
}) {
  return (
    <button
      type="button"
      aria-label={ariaLabel}
      title={title}
      disabled={!enabled}
      className={danger ? "gv-icon-btn gv-icon-btn--danger" : "gv-icon-btn"}
      onClick={(e) => {
        e.stopPropagation();
        onClick();
      }}
    >
      {icon}
    </button>
  );
}

function JobRow({ job, selected, nowMs, onSelect, onCancel, onRetry, onRemove, onMove }: JobRowProps) {
  const percentage = job.progress.percentage;
  const isRunning = job.state === "RUNNING" || job.state === "STARTING";
  const retryIn = secondsUntilRetry(job, nowMs);

  const meta: string[] = [];
  if (job.queuePosition !== undefined) meta.push(`#${job.queuePosition + 1} in queue`);
  if (job.priority !== "NORMAL") meta.push(`${PRIORITY_LABELS[job.priority]} priority`);
  if (isRunning && job.progress.speedBytesPerSecond !== undefined) {
    meta.push(formatSpeed(job.progress.speedBytesPerSecond));
  }
  if (isRunning && job.progress.etaSeconds !== undefined) {
    meta.push(`${formatDuration(job.progress.etaSeconds)} left`);
  }
  if (job.progress.totalBytes !== undefined) {
    meta.push(`${formatBytes(job.progress.processedBytes)} / ${formatBytes(job.progress.totalBytes)}`);
  }
  if (job.attempt > 0) meta.push(`attempt ${job.attempt + 1} of ${job.maxRetries + 1}`);
  if (retryIn !== undefined) meta.push(`retrying in ${retryIn}s`);
  if (job.dependencies.length > 0) meta.push(`${job.dependencies.length} dependency`);

  return (
    <div
      className={selected ? "gv-row gv-row--selected" : "gv-row"}
      // The row is a real button so it is reachable and activatable from the keyboard, with
      // the browser's own focus ring (spec section 18).
      role="button"
      tabIndex={0}
      aria-expanded={selected}
      aria-label={`${JOB_TYPE_LABELS[job.type]}: ${jobTitle(job)}, ${job.state}`}
      onClick={() => onSelect(job.id)}
      onKeyDown={(e) => {
        if (e.key === "Enter" || e.key === " ") {
          e.preventDefault();
          onSelect(job.id);
        }
      }}
    >
      <div className="gv-row__main">
        <div className="gv-row__top">
          <span className="gv-typebadge">{JOB_TYPE_LABELS[job.type]}</span>
          <StatusBadge state={job.state} />
        </div>
        <div className="gv-row__title" title={jobTitle(job)}>
          {jobTitle(job)}
        </div>
        {jobSubtitle(job) ? (
          <div className="gv-row__subtitle" title={jobSubtitle(job)}>
            {jobSubtitle(job)}
          </div>
        ) : null}

        {isRunning ? (
          <div
            className="gv-progress-track"
            role="progressbar"
            aria-valuemin={0}
            aria-valuemax={100}
            aria-valuenow={percentage !== undefined ? Math.round(percentage) : undefined}
            aria-label={`${jobTitle(job)} progress`}
          >
            {percentage !== undefined ? (
              // Clamped so a provider reporting 101% cannot overflow the track, and
              // Math.max(0) so a momentary negative never renders as a backwards jump.
              <div
                className="gv-progress-fill"
                style={{ width: `${Math.min(100, Math.max(0, percentage))}%` }}
              />
            ) : (
              // No percentage available yet (metadata phase, or an operation with no known
              // total). A moving indeterminate bar says "working", not "50% done".
              <div className="gv-progress-indeterminate" />
            )}
          </div>
        ) : null}

        <div className="gv-row__meta">
          {percentage !== undefined && isRunning ? <span>{Math.round(percentage)}%</span> : null}
          <span>{job.progress.statusMessage || undefined}</span>
          {meta.map((item) => (
            <span key={item}>{item}</span>
          ))}
        </div>

        {job.error && (job.state === "FAILED" || job.state === "SKIPPED" || job.state === "RETRY_WAIT") ? (
          <div className="gv-row__error">{describeError(job.error)}</div>
        ) : null}
      </div>

      <div className="gv-row__controls">
        <ControlButton
          icon={<ChevronUpIcon size={14} />}
          ariaLabel={`Move ${jobTitle(job)} up`}
          title={canReorder(job) ? "Move up" : "Only a job that has not started can be moved"}
          enabled={canReorder(job)}
          onClick={() => onMove(job.id, "UP")}
        />
        <ControlButton
          icon={<ChevronDownIcon size={14} />}
          ariaLabel={`Move ${jobTitle(job)} down`}
          title={canReorder(job) ? "Move down" : "Only a job that has not started can be moved"}
          enabled={canReorder(job)}
          onClick={() => onMove(job.id, "DOWN")}
        />
        <ControlButton
          icon={<ChevronToTopIcon size={14} />}
          ariaLabel={`Move ${jobTitle(job)} to top`}
          title={canReorder(job) ? "Move to top" : "Only a job that has not started can be moved"}
          enabled={canReorder(job)}
          onClick={() => onMove(job.id, "TOP")}
        />
        <ControlButton
          icon={<RetryIcon size={14} />}
          ariaLabel={`Retry ${jobTitle(job)}`}
          title={canRetry(job) ? "Run this job again" : "Only a failed or skipped job can be retried"}
          enabled={canRetry(job)}
          onClick={() => onRetry(job.id)}
        />
        <ControlButton
          icon={<CancelIcon size={14} />}
          ariaLabel={`Cancel ${jobTitle(job)}`}
          title={canCancel(job) ? "Stop this job" : "This job has already finished"}
          enabled={canCancel(job)}
          danger
          onClick={() => onCancel(job.id)}
        />
        <ControlButton
          icon={<TrashIcon size={14} />}
          ariaLabel={`Remove ${jobTitle(job)} from the list`}
          title={
            canRemove(job)
              ? "Remove from the list (does not delete any files)"
              : "Only a finished job can be removed from the list"
          }
          enabled={canRemove(job)}
          onClick={() => onRemove(job.id)}
        />
      </div>
    </div>
  );
}

export default memo(JobRow);
