// Home (spec section 5): a launch point and status overview, not a second Queue page. Three
// cards get the user into the three things Gravity does; a compact strip below answers "is
// anything happening right now" without repeating the full queue list.

import { useMemo } from "react";
import { CompressIcon, ConvertIcon, DownloadIcon, InboxIcon, QueueIcon } from "../components/icons";
import { StatusBadge } from "../components/ui/StatusBadge";
import { EmptyState } from "../components/ui/EmptyState";
import type { Route } from "../components/AppShell";
import type { QueueController } from "../state/useQueue";
import { jobTitle } from "../utils/jobDisplay";

interface HomePageProps {
  queue: QueueController;
  onNavigate: (route: Route) => void;
}

const LAUNCH_CARDS: { route: Route; icon: React.ComponentType<{ size?: number }>; title: string; desc: string }[] = [
  {
    route: "download",
    icon: DownloadIcon,
    title: "Download",
    desc: "Paste a URL, inspect the media, and pull it down at the quality you choose.",
  },
  {
    route: "process",
    icon: ConvertIcon,
    title: "Convert",
    desc: "Change a local file's format — video, audio, or image containers.",
  },
  {
    route: "process",
    icon: CompressIcon,
    title: "Compress",
    desc: "Re-encode a local file smaller, in plain-language quality presets.",
  },
];

export default function HomePage({ queue, onNavigate }: HomePageProps) {
  const { state, jobs } = queue;
  const stats = state.statistics;

  const recent = useMemo(
    () =>
      [...jobs]
        .filter((j) => j.state === "RUNNING" || j.state === "COMPLETED" || j.state === "FAILED")
        .sort((a, b) => (b.completedAt ?? b.startedAt ?? b.createdAt).localeCompare(
          a.completedAt ?? a.startedAt ?? a.createdAt,
        ))
        .slice(0, 5),
    [jobs],
  );

  return (
    <div className="gv-enter">
      <h1 className="gv-h1">Gravity</h1>
      <p className="gv-subtitle">Download, convert and compress media, locally. Everything runs through one queue.</p>

      <div className="gv-launch-grid">
        {LAUNCH_CARDS.map(({ route, icon: Glyph, title, desc }) => (
          <button
            key={title}
            type="button"
            className="gv-launch-card"
            onClick={() => onNavigate(route)}
          >
            <span className="gv-launch-card__icon">
              <Glyph size={19} />
            </span>
            <span className="gv-launch-card__title">{title}</span>
            <span className="gv-launch-card__desc">{desc}</span>
          </button>
        ))}
      </div>

      <div style={{ display: "flex", alignItems: "center", marginBottom: "0.75rem" }}>
        <h2 className="gv-section-title" style={{ margin: 0 }}>
          Queue overview
        </h2>
        <div className="gv-spacer" />
        <button type="button" className="gv-btn gv-btn--ghost gv-btn--sm" onClick={() => onNavigate("queue")}>
          <QueueIcon size={14} />
          Open queue
        </button>
      </div>

      <div className="gv-stats-row">
        <div className="gv-stat">
          <div className="gv-stat__value">{stats.running}</div>
          <div className="gv-stat__label">Active</div>
        </div>
        <div className="gv-stat">
          <div className="gv-stat__value">{stats.queued + stats.waiting + stats.retryWait}</div>
          <div className="gv-stat__label">Queued</div>
        </div>
        <div className="gv-stat">
          <div className="gv-stat__value">{stats.completed}</div>
          <div className="gv-stat__label">Completed</div>
        </div>
        <div className="gv-stat">
          <div className="gv-stat__value">{stats.failed}</div>
          <div className="gv-stat__label">Failed</div>
        </div>
      </div>

      {!state.loaded ? (
        <EmptyState icon={<InboxIcon size={28} />} title="Connecting to the core process…" />
      ) : recent.length === 0 ? (
        <EmptyState
          icon={<InboxIcon size={28} />}
          title="Nothing going on yet"
          description="Start a download or a conversion and it will show up here."
          action={
            <button type="button" className="gv-btn gv-btn--secondary gv-btn--sm" onClick={() => onNavigate("download")}>
              Start a download
            </button>
          }
        />
      ) : (
        <div className="gv-list">
          {recent.map((job) => (
            <button
              key={job.id}
              type="button"
              className="gv-row"
              style={{ cursor: "pointer" }}
              onClick={() => onNavigate("queue")}
            >
              <div className="gv-row__main">
                <div className="gv-row__top">
                  <StatusBadge state={job.state} />
                </div>
                <div className="gv-row__title">{jobTitle(job)}</div>
              </div>
            </button>
          ))}
        </div>
      )}
    </div>
  );
}
