// Every job state rendered one consistent way: an icon plus a label, in the state's color.
// Color is never the only signal (spec sections 3, 10, 37) -- a colorblind user, or anyone
// reading a screenshot in grayscale, still gets the word and the shape of the icon.

import type { JobState } from "../../types/job";
import { JOB_STATE_LABELS } from "../../utils/jobDisplay";
import {
  AlertTriangleIcon,
  CancelIcon,
  CheckCircleIcon,
  InfoIcon,
  PauseIcon,
  PlayIcon,
  RetryIcon,
} from "../icons";

const STATE_ICON: Record<JobState, React.ComponentType<{ size?: number }>> = {
  QUEUED: InfoIcon,
  WAITING: InfoIcon,
  STARTING: PlayIcon,
  RUNNING: PlayIcon,
  PAUSED: PauseIcon,
  RETRY_WAIT: RetryIcon,
  RETRYING: RetryIcon,
  COMPLETED: CheckCircleIcon,
  FAILED: AlertTriangleIcon,
  CANCELLED: CancelIcon,
  SKIPPED: CancelIcon,
};

const STATE_CLASS: Record<JobState, string> = {
  QUEUED: "neutral",
  WAITING: "waiting",
  STARTING: "running",
  RUNNING: "running",
  PAUSED: "paused",
  RETRY_WAIT: "retrying",
  RETRYING: "retrying",
  COMPLETED: "completed",
  FAILED: "failed",
  CANCELLED: "neutral",
  SKIPPED: "skipped",
};

export function StatusBadge({ state }: { state: JobState }) {
  const Glyph = STATE_ICON[state];
  const spin = state === "STARTING" || state === "RUNNING" || state === "RETRYING";
  return (
    <span className={`gv-status gv-status--${STATE_CLASS[state]}`}>
      <Glyph size={13} />
      <span className={spin ? "gv-status__pulse" : undefined}>{JOB_STATE_LABELS[state]}</span>
    </span>
  );
}
