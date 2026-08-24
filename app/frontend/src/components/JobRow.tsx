// One job in the queue list. Renders the same shape for every job type -- the differences
// live in jobDisplay's title/subtitle helpers, not in three parallel components.

import type { CSSProperties } from "react";

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
  JOB_STATE_LABELS,
  JOB_TYPE_LABELS,
  PRIORITY_LABELS,
  secondsUntilRetry,
} from "../utils/jobDisplay";
import { STATE_COLORS, styles } from "./queueStyles";

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
  label,
  ariaLabel,
  title,
  enabled,
  danger,
  onClick,
}: {
  label: string;
  ariaLabel: string;
  title: string;
  enabled: boolean;
  danger?: boolean;
  onClick: () => void;
}) {
  const base: CSSProperties = danger ? styles.buttonDanger : styles.iconButton;
  return (
    <button
      type="button"
      aria-label={ariaLabel}
      title={title}
      disabled={!enabled}
      onClick={(e) => {
        e.stopPropagation();
        onClick();
      }}
      style={enabled ? base : { ...base, ...styles.buttonDisabled }}
    >
      {label}
    </button>
  );
}

export default function JobRow({
  job,
  selected,
  nowMs,
  onSelect,
  onCancel,
  onRetry,
  onRemove,
  onMove,
}: JobRowProps) {
  const percentage = job.progress.percentage;
  const isRunning = job.state === "RUNNING" || job.state === "STARTING";
  const retryIn = secondsUntilRetry(job, nowMs);
  const stateColor = STATE_COLORS[job.state];

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
      style={selected ? styles.rowSelected : styles.row}
      // The row is a real button so it is reachable and activatable from the keyboard, with
      // the browser's own focus ring (spec section 37).
      role="button"
      tabIndex={0}
      aria-expanded={selected}
      aria-label={`${JOB_TYPE_LABELS[job.type]}: ${jobTitle(job)}, ${JOB_STATE_LABELS[job.state]}`}
      onClick={() => onSelect(job.id)}
      onKeyDown={(e) => {
        if (e.key === "Enter" || e.key === " ") {
          e.preventDefault();
          onSelect(job.id);
        }
      }}
    >
      <div style={styles.rowMain}>
        <div style={{ display: "flex", gap: "0.4rem", alignItems: "center", marginBottom: "0.15rem" }}>
          <span style={styles.typeBadge}>{JOB_TYPE_LABELS[job.type]}</span>
          {/* Colour is paired with the text label, never the only signal. */}
          <span style={{ ...styles.badge, color: stateColor }}>{JOB_STATE_LABELS[job.state]}</span>
        </div>
        <div style={styles.rowTitle} title={jobTitle(job)}>
          {jobTitle(job)}
        </div>
        {jobSubtitle(job) ? (
          <div style={styles.rowSubtitle} title={jobSubtitle(job)}>
            {jobSubtitle(job)}
          </div>
        ) : null}

        {isRunning ? (
          <div
            style={styles.progressTrack}
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
                style={{ ...styles.progressFill, width: `${Math.min(100, Math.max(0, percentage))}%` }}
              />
            ) : (
              // No percentage available yet (metadata phase, or an operation with no known
              // total). A neutral bar is honest; a fake moving one is not.
              <div style={styles.progressIndeterminate} />
            )}
          </div>
        ) : null}

        <div style={{ ...styles.rowMeta, marginTop: "0.35rem" }}>
          {percentage !== undefined && isRunning ? <span>{Math.round(percentage)}%</span> : null}
          <span>{job.progress.statusMessage || JOB_STATE_LABELS[job.state]}</span>
          {meta.map((item) => (
            <span key={item}>{item}</span>
          ))}
        </div>

        {job.error && (job.state === "FAILED" || job.state === "SKIPPED" || job.state === "RETRY_WAIT") ? (
          <div style={{ ...styles.rowSubtitle, color: STATE_COLORS[job.state], marginTop: "0.25rem" }}>
            {describeError(job.error)}
          </div>
        ) : null}
      </div>

      <div style={styles.rowControls}>
        <ControlButton
          label="↑"
          ariaLabel={`Move ${jobTitle(job)} up`}
          title={canReorder(job) ? "Move up" : "Only a job that has not started can be moved"}
          enabled={canReorder(job)}
          onClick={() => onMove(job.id, "UP")}
        />
        <ControlButton
          label="↓"
          ariaLabel={`Move ${jobTitle(job)} down`}
          title={canReorder(job) ? "Move down" : "Only a job that has not started can be moved"}
          enabled={canReorder(job)}
          onClick={() => onMove(job.id, "DOWN")}
        />
        <ControlButton
          label="⤒"
          ariaLabel={`Move ${jobTitle(job)} to top`}
          title={canReorder(job) ? "Move to top" : "Only a job that has not started can be moved"}
          enabled={canReorder(job)}
          onClick={() => onMove(job.id, "TOP")}
        />
        <ControlButton
          label="Retry"
          ariaLabel={`Retry ${jobTitle(job)}`}
          title={canRetry(job) ? "Run this job again" : "Only a failed or skipped job can be retried"}
          enabled={canRetry(job)}
          onClick={() => onRetry(job.id)}
        />
        <ControlButton
          label="Cancel"
          ariaLabel={`Cancel ${jobTitle(job)}`}
          title={canCancel(job) ? "Stop this job" : "This job has already finished"}
          enabled={canCancel(job)}
          danger
          onClick={() => onCancel(job.id)}
        />
        <ControlButton
          label="✕"
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
