// The download workflow (spec section 6): paste a URL, inspect it, see what it is, choose
// quality and destination, queue it, and watch it land in the shared queue. Every step has
// an explicit UI state -- idle, inspecting, ready, invalid URL, playlist, network failure,
// no formats -- rather than one screen that silently does or doesn't have data.

import { useCallback, useMemo, useState } from "react";
import * as coreClient from "../services/coreClient";
import { asErrorInfo } from "../utils/errors";
import { describeError, formatBytes, formatDuration, formatSpeed } from "../utils/jobDisplay";
import type { DownloadMetadata, QualityPreset } from "../types/download";
import { QUALITY_PRESET_LABELS } from "../types/download";
import type { ErrorInfo } from "../types/error";
import type { Route } from "../components/AppShell";
import type { QueueController } from "../state/useQueue";
import {
  AlertTriangleIcon,
  CheckCircleIcon,
  DownloadIcon,
  FolderIcon,
  InfoIcon,
  LinkIcon,
  QueueIcon,
  SpinnerIcon,
} from "../components/icons";
import { Button } from "../components/ui/Button";
import { StatusBadge } from "../components/ui/StatusBadge";

const QUALITY_OPTIONS: QualityPreset[] = ["BEST", "2160P", "1440P", "1080P", "720P", "480P", "AUDIO_ONLY"];

type InspectStage = "idle" | "inspecting" | "ready" | "error";

function ErrorBanner({ error, title }: { error: ErrorInfo | null | undefined; title?: string }) {
  if (!error) return null;
  return (
    <div className="gv-banner gv-banner--error" role="alert">
      <AlertTriangleIcon size={15} />
      <div className="gv-banner__body">
        <div className="gv-banner__title">{title ?? describeError(error)}</div>
        {title ? <div className="gv-banner__detail">{describeError(error)}</div> : null}
        {error.details && error.details !== error.message ? (
          <details style={{ marginTop: "0.3rem" }}>
            <summary style={{ cursor: "pointer", fontSize: "0.75rem" }}>Technical details</summary>
            <div className="gv-banner__detail">{error.details}</div>
          </details>
        ) : null}
      </div>
    </div>
  );
}

interface DownloaderPageProps {
  queue: QueueController;
  onNavigate: (route: Route) => void;
}

export default function DownloaderPage({ queue, onNavigate }: DownloaderPageProps) {
  const [url, setUrl] = useState("");
  const [stage, setStage] = useState<InspectStage>("idle");
  const [inspectError, setInspectError] = useState<ErrorInfo | null>(null);
  const [metadata, setMetadata] = useState<DownloadMetadata | null>(null);

  const [quality, setQuality] = useState<QualityPreset>("BEST");
  const [outputDirectory, setOutputDirectory] = useState("");

  const [creating, setCreating] = useState(false);
  const [createError, setCreateError] = useState<ErrorInfo | null>(null);
  const [activeJobId, setActiveJobId] = useState<string | null>(null);
  const [cancelBusy, setCancelBusy] = useState(false);

  const activeJob = activeJobId ? queue.state.jobs[activeJobId] : undefined;
  const canCancel = activeJob !== undefined && ["QUEUED", "STARTING", "RUNNING", "RETRY_WAIT", "RETRYING"].includes(activeJob.state);
  const isPlaylist = metadata !== null && (metadata.playlistCount ?? 0) > 1;
  const noFormats = metadata !== null && metadata.formats.length === 0;
  const canStartDownload =
    metadata !== null && !noFormats && outputDirectory.trim().length > 0 && !creating && !canCancel;

  const looksLikeUrl = useMemo(() => {
    const trimmed = url.trim();
    if (!trimmed) return false;
    try {
      const parsed = new URL(trimmed);
      return parsed.protocol === "http:" || parsed.protocol === "https:";
    } catch {
      return false;
    }
  }, [url]);

  const handleInspect = useCallback(async () => {
    setStage("inspecting");
    setInspectError(null);
    setMetadata(null);
    try {
      const { metadata: result } = await coreClient.inspectDownloadUrl(url.trim());
      setMetadata(result);
      setStage("ready");
    } catch (err) {
      setInspectError(asErrorInfo(err));
      setStage("error");
    }
  }, [url]);

  const handleDownload = useCallback(async () => {
    setCreating(true);
    setCreateError(null);
    try {
      const { jobId } = await coreClient.createDownloadJob({
        url: url.trim(),
        outputDirectory: outputDirectory.trim(),
        quality,
      });
      setActiveJobId(jobId);
    } catch (err) {
      setCreateError(asErrorInfo(err));
    } finally {
      setCreating(false);
    }
  }, [url, outputDirectory, quality]);

  const handleCancel = useCallback(async () => {
    if (!activeJobId) return;
    setCancelBusy(true);
    try {
      await queue.cancelJob(activeJobId);
    } finally {
      setCancelBusy(false);
    }
  }, [activeJobId, queue]);

  const handleOpenFolder = useCallback(async () => {
    const outputPath = activeJob?.result?.outputPath;
    if (typeof outputPath !== "string") return;
    try {
      await coreClient.openContainingFolder(outputPath);
    } catch {
      /* best-effort; the file still exists even if reveal fails */
    }
  }, [activeJob]);

  const handleStartOver = useCallback(() => {
    setActiveJobId(null);
    setMetadata(null);
    setStage("idle");
    setUrl("");
    setCreateError(null);
  }, []);

  return (
    <div className="gv-enter">
      <h1 className="gv-h1">Download</h1>
      <p className="gv-subtitle">Paste a link, inspect what's there, then choose quality and destination.</p>

      <section className="gv-panel" style={{ marginBottom: "1.25rem" }}>
        <div className="gv-field">
          <label className="gv-label" htmlFor="downloadUrl">
            Media URL
          </label>
          <div style={{ display: "flex", gap: "0.5rem" }}>
            <input
              id="downloadUrl"
              className="gv-input"
              type="text"
              placeholder="https://…"
              value={url}
              onChange={(e) => {
                setUrl(e.target.value);
                setStage("idle");
              }}
              disabled={canCancel}
              onKeyDown={(e) => {
                if (e.key === "Enter" && looksLikeUrl && stage !== "inspecting") void handleInspect();
              }}
            />
            <Button
              variant="primary"
              busy={stage === "inspecting"}
              disabled={!looksLikeUrl || canCancel}
              icon={<LinkIcon size={15} />}
              onClick={() => void handleInspect()}
            >
              Inspect
            </Button>
          </div>
          {url.trim() && !looksLikeUrl ? (
            <span className="gv-hint" style={{ color: "var(--status-failed)" }}>
              That doesn't look like a web address yet.
            </span>
          ) : null}
        </div>

        {stage === "error" ? (
          <div style={{ marginTop: "1rem" }}>
            <ErrorBanner error={inspectError} title="Couldn't inspect that link" />
          </div>
        ) : null}

        {stage === "ready" && metadata ? (
          <div style={{ marginTop: "1rem" }}>
            {isPlaylist ? (
              <div className="gv-banner gv-banner--info" role="note">
                <InfoIcon size={15} />
                <div className="gv-banner__body">
                  <div className="gv-banner__title">This is a playlist</div>
                  <div className="gv-banner__detail">
                    {metadata.playlistCount} items detected. Gravity downloads the first item this
                    page links to; queue each item separately for the rest.
                  </div>
                </div>
              </div>
            ) : null}
            {noFormats ? (
              <div className="gv-banner gv-banner--warning" role="alert">
                <AlertTriangleIcon size={15} />
                <div className="gv-banner__body">
                  <div className="gv-banner__title">No downloadable formats found</div>
                  <div className="gv-banner__detail">
                    Gravity found this page but couldn't find a media stream on it.
                  </div>
                </div>
              </div>
            ) : null}
            <div className="gv-card" style={{ display: "flex", gap: "0.9rem", alignItems: "flex-start" }}>
              {metadata.thumbnailUrl ? (
                <img
                  src={metadata.thumbnailUrl}
                  alt=""
                  style={{ width: 128, borderRadius: 6, flexShrink: 0 }}
                  onError={(e) => {
                    (e.currentTarget as HTMLImageElement).style.display = "none";
                  }}
                />
              ) : null}
              <div style={{ minWidth: 0 }}>
                <div style={{ fontWeight: 600, fontSize: "var(--text-base)" }}>{metadata.title}</div>
                {metadata.uploader ? (
                  <div style={{ color: "var(--text-secondary)", fontSize: "var(--text-sm)" }}>
                    {metadata.uploader}
                  </div>
                ) : null}
                <div className="gv-row__meta" style={{ marginTop: "0.4rem" }}>
                  <span>{formatDuration(metadata.durationSeconds)}</span>
                  <span>
                    {metadata.formats.length} format{metadata.formats.length === 1 ? "" : "s"} available
                  </span>
                </div>
              </div>
            </div>
          </div>
        ) : null}
      </section>

      {stage === "ready" && metadata && !noFormats ? (
        <section className="gv-panel" style={{ marginBottom: "1.25rem" }}>
          <h2 className="gv-section-title">Download options</h2>
          <div className="gv-grid-2">
            <div className="gv-field">
              <label className="gv-label" htmlFor="quality">
                Quality
              </label>
              <select
                id="quality"
                className="gv-select"
                value={quality}
                onChange={(e) => setQuality(e.target.value as QualityPreset)}
                disabled={canCancel}
              >
                {QUALITY_OPTIONS.map((preset) => (
                  <option key={preset} value={preset}>
                    {QUALITY_PRESET_LABELS[preset]}
                  </option>
                ))}
              </select>
            </div>
            <div className="gv-field">
              <label className="gv-label" htmlFor="outputDirectory">
                Destination folder
              </label>
              <input
                id="outputDirectory"
                className="gv-input"
                type="text"
                placeholder="Choose a folder"
                value={outputDirectory}
                onChange={(e) => setOutputDirectory(e.target.value)}
                disabled={canCancel}
              />
            </div>
          </div>
          <div style={{ marginTop: "1rem", display: "flex", gap: "0.5rem" }}>
            <Button
              variant="primary"
              busy={creating}
              disabled={!canStartDownload}
              icon={<DownloadIcon size={15} />}
              onClick={() => void handleDownload()}
            >
              Download
            </Button>
            {canCancel ? (
              <Button variant="destructive" busy={cancelBusy} onClick={() => void handleCancel()}>
                Cancel
              </Button>
            ) : null}
          </div>
          <div style={{ marginTop: "0.75rem" }}>
            <ErrorBanner error={createError} title="Couldn't start the download" />
          </div>
        </section>
      ) : null}

      {activeJob ? (
        <section className="gv-panel gv-enter">
          <div style={{ display: "flex", alignItems: "center", gap: "0.6rem", marginBottom: "0.75rem" }}>
            <h2 className="gv-section-title" style={{ margin: 0 }}>
              This download
            </h2>
            <StatusBadge state={activeJob.state} />
          </div>

          {activeJob.state === "RUNNING" || activeJob.state === "STARTING" ? (
            <>
              <div className="gv-progress-track">
                {activeJob.progress.percentage !== undefined ? (
                  <div
                    className="gv-progress-fill"
                    style={{ width: `${Math.min(100, Math.max(0, activeJob.progress.percentage))}%` }}
                  />
                ) : (
                  <div className="gv-progress-indeterminate" />
                )}
              </div>
              <div className="gv-row__meta" style={{ marginTop: "0.5rem" }}>
                <span>{activeJob.progress.statusMessage || "Working…"}</span>
                {activeJob.progress.totalBytes !== undefined ? (
                  <span>
                    {formatBytes(activeJob.progress.processedBytes)} / {formatBytes(activeJob.progress.totalBytes)}
                  </span>
                ) : null}
                {activeJob.progress.speedBytesPerSecond !== undefined ? (
                  <span>{formatSpeed(activeJob.progress.speedBytesPerSecond)}</span>
                ) : null}
                {activeJob.progress.etaSeconds !== undefined ? (
                  <span>ETA {formatDuration(activeJob.progress.etaSeconds)}</span>
                ) : null}
              </div>
            </>
          ) : null}

          {activeJob.state === "QUEUED" || activeJob.state === "WAITING" ? (
            <div style={{ display: "flex", alignItems: "center", gap: "0.5rem", color: "var(--text-secondary)", fontSize: "var(--text-sm)" }}>
              <SpinnerIcon size={14} />
              Waiting for a free slot in the queue.
            </div>
          ) : null}

          {activeJob.state === "COMPLETED" ? (
            <div className="gv-banner gv-banner--success" role="status" style={{ marginBottom: 0 }}>
              <CheckCircleIcon size={16} />
              <div className="gv-banner__body">
                <div className="gv-banner__title">Download complete</div>
                {typeof activeJob.result?.outputPath === "string" ? (
                  <div className="gv-banner__detail">{activeJob.result.outputPath}</div>
                ) : null}
                <div style={{ display: "flex", gap: "0.5rem", marginTop: "0.6rem" }}>
                  <Button variant="secondary" size="sm" icon={<FolderIcon size={14} />} onClick={() => void handleOpenFolder()}>
                    Open folder
                  </Button>
                  <Button variant="ghost" size="sm" onClick={handleStartOver}>
                    Download another
                  </Button>
                </div>
              </div>
            </div>
          ) : null}

          {activeJob.state === "FAILED" && activeJob.error ? (
            <div>
              <ErrorBanner error={activeJob.error} title="This download failed" />
              <div style={{ display: "flex", gap: "0.5rem" }}>
                <Button variant="secondary" size="sm" onClick={handleStartOver}>
                  Try again
                </Button>
                <Button variant="ghost" size="sm" icon={<QueueIcon size={14} />} onClick={() => onNavigate("queue")}>
                  View in queue
                </Button>
              </div>
            </div>
          ) : null}

          {activeJob.state === "CANCELLED" ? (
            <div>
              <p style={{ color: "var(--text-secondary)", fontSize: "var(--text-sm)" }}>Download cancelled.</p>
              <Button variant="secondary" size="sm" onClick={handleStartOver}>
                Start over
              </Button>
            </div>
          ) : null}
        </section>
      ) : null}
    </div>
  );
}
