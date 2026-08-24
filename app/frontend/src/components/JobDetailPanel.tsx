// Everything known about one job (spec section 11). Shows diagnostics that help -- the id,
// the error code, the dependency ids -- and deliberately not the internals that do not: no
// ffmpeg argv, no yt-dlp command line, no raw stderr.

import type { ReactNode } from "react";
import type { JobPriority, JobSnapshot } from "../types/job";
import {
  canChangePriority,
  describeError,
  formatBytes,
  formatDuration,
  formatSpeed,
  formatTimestamp,
  jobSubtitle,
  jobTitle,
  JOB_TYPE_LABELS,
  PRIORITY_LABELS,
  secondsUntilRetry,
} from "../utils/jobDisplay";
import { StatusBadge } from "./ui/StatusBadge";
import { AlertTriangleIcon, XIcon } from "./icons";

interface JobDetailPanelProps {
  job: JobSnapshot;
  nowMs: number;
  onSetPriority: (jobId: string, priority: JobPriority) => void;
  onClose: () => void;
}

function Field({ label, value }: { label: string; value: ReactNode }) {
  if (value === undefined || value === null || value === "") return null;
  return (
    <>
      <div className="gv-detail__label">{label}</div>
      <div className="gv-detail__value">{value}</div>
    </>
  );
}

function metaString(job: JobSnapshot, key: string): string | undefined {
  const value = job.metadata?.[key];
  return typeof value === "string" && value.length > 0 ? value : undefined;
}

export default function JobDetailPanel({ job, nowMs, onSetPriority, onClose }: JobDetailPanelProps) {
  const retryIn = secondsUntilRetry(job, nowMs);
  const resultPath = typeof job.result?.outputPath === "string" ? job.result.outputPath : undefined;
  const priorityDisabled = !canChangePriority(job);

  return (
    <section className="gv-panel gv-detail" aria-label={`Details for ${jobTitle(job)}`}>
      <div className="gv-detail__header">
        <h2 style={{ fontSize: "var(--text-base)", margin: 0 }}>{jobTitle(job)}</h2>
        <div className="gv-spacer" />
        <StatusBadge state={job.state} />
        <button
          type="button"
          className="gv-icon-btn"
          style={{ marginLeft: "0.75rem" }}
          onClick={onClose}
          aria-label="Close job details"
        >
          <XIcon size={14} />
        </button>
      </div>

      <div className="gv-detail__grid">
        <Field label="Job ID" value={<code>{job.id}</code>} />
        <Field label="Type" value={JOB_TYPE_LABELS[job.type]} />
        <Field label="Operation" value={jobSubtitle(job)} />

        <div className="gv-detail__label">Priority</div>
        <div className="gv-detail__value">
          <label className="sr-only" htmlFor={`priority-${job.id}`}>
            Priority for {jobTitle(job)}
          </label>
          <select
            id={`priority-${job.id}`}
            className="gv-select"
            style={{ width: "auto" }}
            value={job.priority}
            disabled={priorityDisabled}
            title={
              priorityDisabled
                ? "A finished job's priority cannot be changed"
                : "Changing priority reorders pending work; it never restarts a running job"
            }
            onChange={(e) => onSetPriority(job.id, e.target.value as JobPriority)}
          >
            {(["LOW", "NORMAL", "HIGH"] as JobPriority[]).map((value) => (
              <option key={value} value={value}>
                {PRIORITY_LABELS[value]}
              </option>
            ))}
          </select>
        </div>

        <Field label="Created" value={formatTimestamp(job.createdAt)} />
        <Field label="Started" value={formatTimestamp(job.startedAt)} />
        <Field label="Finished" value={formatTimestamp(job.completedAt)} />

        <Field label="Source" value={metaString(job, "inputPath") ?? metaString(job, "webpageUrl")} />
        <Field label="Destination" value={resultPath ?? metaString(job, "outputPath")} />
        <Field label="Source format" value={metaString(job, "sourceFormat")?.toUpperCase()} />
        <Field label="Target format" value={metaString(job, "targetFormat")?.toUpperCase()} />
        <Field label="Preset" value={metaString(job, "preset")} />
        <Field label="Quality" value={metaString(job, "quality")} />

        <Field
          label="Progress"
          value={
            job.progress.percentage !== undefined
              ? `${Math.round(job.progress.percentage)}% — ${job.progress.statusMessage}`
              : job.progress.statusMessage
          }
        />
        <Field
          label="Speed"
          value={
            job.progress.speedBytesPerSecond !== undefined
              ? formatSpeed(job.progress.speedBytesPerSecond)
              : undefined
          }
        />
        <Field
          label="ETA"
          value={job.progress.etaSeconds !== undefined ? formatDuration(job.progress.etaSeconds) : undefined}
        />
        <Field
          label="Transferred"
          value={
            job.progress.totalBytes !== undefined
              ? `${formatBytes(job.progress.processedBytes)} of ${formatBytes(job.progress.totalBytes)}`
              : undefined
          }
        />

        <Field
          label="Attempts"
          value={job.maxRetries > 0 || job.attempt > 0 ? `${job.attempt + 1} of ${job.maxRetries + 1}` : undefined}
        />
        <Field label="Next retry" value={retryIn !== undefined ? `in ${retryIn}s` : undefined} />
        <Field label="Retry reason" value={job.retryReason} />

        <Field
          label="Depends on"
          value={
            job.dependencies.length > 0 ? (
              <ul style={{ margin: 0, paddingLeft: "1.1rem" }}>
                {job.dependencies.map((id) => (
                  <li key={id}>
                    <code>{id}</code>
                  </li>
                ))}
              </ul>
            ) : undefined
          }
        />
        <Field label="Part of" value={job.parentJobId ? <code>{job.parentJobId}</code> : undefined} />

        {job.error ? (
          <>
            <div className="gv-detail__label">Error</div>
            <div className="gv-detail__value">
              <div style={{ display: "flex", alignItems: "flex-start", gap: "0.4rem" }}>
                <AlertTriangleIcon size={14} style={{ color: "var(--status-failed)", flexShrink: 0, marginTop: 2 }} />
                <div>
                  <div>{describeError(job.error)}</div>
                  {/* The code is shown as a diagnostic the user can quote in a bug report;
                      the sentence above it is what they are meant to read. */}
                  <div style={{ color: "var(--text-tertiary)", fontSize: "var(--text-xs)", marginTop: "0.2rem" }}>
                    {job.error.code} ({job.error.category})
                    {job.error.recoverable ? " — this one can be retried" : ""}
                  </div>
                  {job.error.details && job.error.details !== job.error.message ? (
                    <details style={{ marginTop: "0.3rem" }}>
                      <summary style={{ cursor: "pointer", fontSize: "var(--text-xs)" }}>Technical details</summary>
                      <pre style={{ whiteSpace: "pre-wrap", fontSize: "var(--text-xs)", margin: "0.3rem 0 0" }}>
                        {job.error.details}
                      </pre>
                    </details>
                  ) : null}
                </div>
              </div>
            </div>
          </>
        ) : null}
      </div>
    </section>
  );
}
