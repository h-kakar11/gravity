import { useEffect, useState } from "react";
import GlassCard from "../components/GlassCard";
import { useJobs } from "../hooks/useJobs";
import * as coreClient from "../services/coreClient";
import type { JobSnapshot } from "../types/job";
import { asErrorInfo } from "../utils/errors";
import { formatBytesPerSecond, formatEta, formatTimestamp } from "../utils/format";
import styles from "./QueuePage.module.css";

const TERMINAL_STATES = new Set(["COMPLETED", "FAILED", "CANCELLED"]);

function JobTypeLabel({ type }: { type: JobSnapshot["type"] }) {
  const label = type === "CONVERSION" ? "Convert" : type === "COMPRESSION" ? "Compress" : type === "DOWNLOAD" ? "Download" : type;
  return <span className={styles.typeBadge}>{label}</span>;
}

function ActiveJobRow({
  job,
  onCancel,
  onPause,
  onResume,
  onRetry,
}: {
  job: JobSnapshot;
  onCancel: (id: string) => void;
  onPause: (id: string) => void;
  onResume: (id: string) => void;
  onRetry: (id: string) => void;
}) {
  const percentage = job.progress.percentage ?? 0;
  const speed = formatBytesPerSecond(job.progress.speedBytesPerSecond);
  const eta = formatEta(job.progress.etaSeconds);

  return (
    <GlassCard className={styles.row}>
      <div className={styles.rowMain}>
        <div className={styles.rowTitle}>
          <JobTypeLabel type={job.type} />
          <span className={styles.statusText}>{job.progress.statusMessage}</span>
        </div>
        {job.progress.percentage !== undefined && (
          <div className={styles.progressTrack}>
            <div className={styles.progressFill} style={{ width: `${Math.min(100, Math.max(0, percentage))}%` }} />
          </div>
        )}
        <div className={styles.metaRow}>
          {speed && <span>{speed}</span>}
          {eta && <span>{eta}</span>}
          {job.progress.percentage !== undefined && <span>{Math.round(percentage)}%</span>}
        </div>
      </div>
      <div className={styles.actions}>
        {job.state === "RUNNING" && (
          <button className={styles.actionButton} onClick={() => onPause(job.id)}>
            Pause
          </button>
        )}
        {job.state === "PAUSED" && (
          <button className={styles.actionButton} onClick={() => onResume(job.id)}>
            Resume
          </button>
        )}
        {job.state === "FAILED" && (
          <button className={styles.actionButton} onClick={() => onRetry(job.id)}>
            Retry
          </button>
        )}
        {job.state !== "FAILED" && (
          <button className={styles.actionButton} onClick={() => onCancel(job.id)}>
            Cancel
          </button>
        )}
      </div>
    </GlassCard>
  );
}

function HistoryRow({ job }: { job: JobSnapshot }) {
  const outputPath =
    typeof job.result?.outputPath === "string" ? (job.result.outputPath as string) : undefined;
  const [copied, setCopied] = useState(false);

  const copyPath = async () => {
    if (!outputPath) return;
    try {
      await navigator.clipboard.writeText(outputPath);
      setCopied(true);
      setTimeout(() => setCopied(false), 1500);
    } catch {
      // Clipboard access denied -- nothing more we can do here.
    }
  };

  const openFolder = async () => {
    if (!outputPath) return;
    try {
      await coreClient.openContainingFolder(outputPath);
    } catch {
      // Best-effort; not worth surfacing a whole error banner for a reveal action.
    }
  };

  return (
    <GlassCard className={styles.row}>
      <div className={styles.rowMain}>
        <div className={styles.rowTitle}>
          <JobTypeLabel type={job.type} />
          <span className={styles.statusText}>
            {job.state === "COMPLETED" ? outputPath ?? "Completed" : job.state === "CANCELLED" ? "Cancelled" : "Failed"}
          </span>
        </div>
        <div className={styles.metaRow}>
          <span>{formatTimestamp(job.completedAt ?? job.createdAt)}</span>
        </div>
        {job.state === "FAILED" && job.error && <div className={styles.errorText}>{job.error.message}</div>}
      </div>
      {job.state === "COMPLETED" && outputPath && (
        <div className={styles.actions}>
          <button className={styles.actionButton} onClick={copyPath}>
            {copied ? "Copied" : "Copy path"}
          </button>
          <button className={styles.actionButton} onClick={openFolder}>
            Show in folder
          </button>
        </div>
      )}
    </GlassCard>
  );
}

export default function QueuePage() {
  const { jobs, connectionError, cancelJob, pauseJob, resumeJob, retryJob, removeJob } = useJobs();
  const [history, setHistory] = useState<JobSnapshot[]>([]);
  const [historyError, setHistoryError] = useState<string | null>(null);

  const refreshHistory = () => {
    coreClient
      .listJobHistory(50)
      .then(({ jobs: entries }) => setHistory(entries))
      .catch((err) => setHistoryError(asErrorInfo(err).message));
  };

  useEffect(() => {
    refreshHistory();
    const unsubscribe = coreClient.subscribeToJobEvents((event) => {
      if (event.event === "jobCompleted" || event.event === "jobFailed" || event.event === "jobCancelled") {
        refreshHistory();
      }
    });
    return unsubscribe;
  }, []);

  const activeJobs = jobs
    .filter((job) => !TERMINAL_STATES.has(job.state))
    .sort((a, b) => a.createdAt.localeCompare(b.createdAt));

  // Issue #29: terminal jobs stay in JobManager's in-memory set (unbounded growth over a
  // long session) until something calls removeJob -- the History section below is driven
  // by the separate, already-bounded JobHistoryStore and is unaffected either way, so this
  // is purely backend cleanup, not a visual change to History.
  const clearableCount = jobs.filter((job) => TERMINAL_STATES.has(job.state)).length;
  const clearCompleted = () => {
    for (const job of jobs) {
      if (TERMINAL_STATES.has(job.state)) void removeJob(job.id);
    }
  };

  return (
    <div className={styles.wrap}>
      {connectionError && <div className={styles.errorText}>{connectionError}</div>}

      <section>
        <h2 className={styles.sectionTitle}>Active ({activeJobs.length})</h2>
        {activeJobs.length === 0 ? (
          <div className={styles.emptyState}>Nothing running right now.</div>
        ) : (
          <div className={styles.list}>
            {activeJobs.map((job) => (
              <ActiveJobRow
                key={job.id}
                job={job}
                onCancel={(id) => void cancelJob(id)}
                onPause={(id) => void pauseJob(id)}
                onResume={(id) => void resumeJob(id)}
                onRetry={(id) => void retryJob(id)}
              />
            ))}
          </div>
        )}
      </section>

      <section>
        <div className={styles.historyHeader}>
          <h2 className={styles.sectionTitle}>History</h2>
          {clearableCount > 0 && (
            <button className={styles.actionButton} onClick={clearCompleted}>
              Clear completed ({clearableCount})
            </button>
          )}
        </div>
        {historyError && <div className={styles.errorText}>{historyError}</div>}
        {history.length === 0 ? (
          <div className={styles.emptyState}>No completed jobs yet.</div>
        ) : (
          <div className={styles.list}>
            {history.map((job) => (
              <HistoryRow key={job.id} job={job} />
            ))}
          </div>
        )}
      </section>
    </div>
  );
}
