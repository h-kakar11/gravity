import { useCallback, useEffect, useMemo, useState, type CSSProperties } from "react";
import PresetBar from "../components/PresetBar";
import { useJobs } from "../hooks/useJobs";
import { useNavigation } from "../navigation/NavigationContext";
import * as coreClient from "../services/coreClient";
import { asErrorInfo } from "../utils/errors";
import { formatSpeedWithUnit } from "../utils/format";
import type { DownloadMetadata, QualityPreset } from "../types/download";
import { QUALITY_PRESET_LABELS } from "../types/download";
import type { ErrorInfo } from "../types/error";
import type { JobState } from "../types/job";
import type { SpeedUnit } from "../types/settings";

// Functional Phase 2 downloader screen (spec section 34) -- proves URL -> metadata ->
// quality selection -> real download -> progress -> cancellation -> verified completion
// end-to-end through the real architecture (React -> Tauri -> C++ -> DownloadJob ->
// YtDlpProvider -> Python -> yt-dlp). Deliberately basic styling; the final dark-mode home
// screen (docs/roadmap.md "UI") is a later phase.

const QUALITY_OPTIONS: QualityPreset[] = ["BEST", "2160P", "1440P", "1080P", "720P", "480P", "AUDIO_ONLY"];
const ACTIVE_STATES: ReadonlySet<JobState> = new Set(["QUEUED", "STARTING", "RUNNING", "PAUSED"]);

function formatBytes(bytes: number | undefined): string {
  if (bytes === undefined) return "?";
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(0)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

function formatEta(seconds: number | undefined): string {
  if (seconds === undefined || !Number.isFinite(seconds)) return "?";
  const total = Math.max(0, Math.round(seconds));
  const mm = Math.floor(total / 60);
  const ss = total % 60;
  return `${mm}:${ss.toString().padStart(2, "0")}`;
}

function formatDuration(seconds: number | undefined): string {
  if (seconds === undefined) return "?";
  return formatEta(seconds);
}

// A dedicated, plain-language treatment for connectivity failures (issue #55) instead of
// the generic banner's raw category/code -- these are the one error class a user can
// usually just retry once whatever's interrupting their connection clears up.
function NetworkErrorBanner() {
  return (
    <div style={styles.errorBanner} role="alert">
      Can&apos;t reach the network. Check your internet connection and try again.
    </div>
  );
}

function ErrorBanner({ error }: { error: ErrorInfo | null | undefined }) {
  if (!error) return null;
  if (error.category === "NETWORK_ERROR") return <NetworkErrorBanner />;
  return (
    <div style={styles.errorBanner} role="alert">
      <strong>{error.category}</strong> ({error.code}): {error.message}
      {error.details && error.details !== error.message ? (
        // Raw diagnostic text (can be a full Python traceback, per downloader.py's
        // emit_error) collapsed behind a disclosure instead of always shown -- issue #33.
        <details style={styles.errorDetails}>
          <summary>Technical details</summary>
          <div>{error.details}</div>
        </details>
      ) : null}
    </div>
  );
}

export default function DownloaderPage() {
  const { jobs, cancelJob } = useJobs();
  const { screen } = useNavigation();
  // Prefilled by HomePage's URL-less nav (nothing today) and by the paste-and-download
  // global hotkey (App.tsx), which navigates here with the clipboard's URL already in hand.
  const prefillUrl = screen.kind === "download" ? screen.prefillUrl : undefined;

  const [url, setUrl] = useState(prefillUrl ?? "");

  // The page may already be mounted (user already on the Download screen) when a new
  // paste-and-download hotkey event lands -- the initial useState above only covers a
  // fresh mount, so re-sync whenever a new prefillUrl arrives.
  useEffect(() => {
    if (prefillUrl) setUrl(prefillUrl);
  }, [prefillUrl]);
  const [inspecting, setInspecting] = useState(false);
  const [inspectError, setInspectError] = useState<ErrorInfo | null>(null);
  const [metadata, setMetadata] = useState<DownloadMetadata | null>(null);
  // Picking a specific stream from the format list overrides the quality preset for that
  // download (issue #31) -- null means "use the quality preset instead", same as leaving
  // it unset always did.
  const [selectedFormatId, setSelectedFormatId] = useState<string | null>(null);

  const [quality, setQuality] = useState<QualityPreset>("BEST");
  const [outputDirectory, setOutputDirectory] = useState("");
  const [speedUnit, setSpeedUnit] = useState<SpeedUnit>("MBps");

  // Seed from Settings once, same as ConvertPage.tsx -- this page never did, so it always
  // started blank regardless of the user's configured default (issue #54), and the quality
  // selector always started at "BEST" regardless of downloads.defaultQuality (part of
  // issue #18). The backend stores defaultQuality lowercase ("best"); validate + uppercase
  // before trusting it as a QualityPreset rather than assuming the stored value is already
  // one of the known presets. downloads.speedUnits was persisted and shown in Settings but
  // never actually consumed anywhere -- the progress display always hardcoded MB/s
  // regardless (also part of #18/#59). The user can still override quality/output dir per
  // download; speed units stay a Settings-level choice, same as elsewhere in the app.
  useEffect(() => {
    coreClient
      .getSettings()
      .then(({ settings }) => {
        setOutputDirectory(settings.general.defaultOutputDirectory);
        const upper = settings.downloads.defaultQuality.toUpperCase() as QualityPreset;
        if (QUALITY_OPTIONS.includes(upper)) setQuality(upper);
        setSpeedUnit(settings.downloads.speedUnits);
      })
      .catch(() => {
        // Non-fatal -- the fields just start at their existing defaults.
      });
  }, []);

  const [creating, setCreating] = useState(false);
  const [createError, setCreateError] = useState<ErrorInfo | null>(null);
  const [activeJobId, setActiveJobId] = useState<string | null>(null);
  const [cancelBusy, setCancelBusy] = useState(false);
  const [openFolderError, setOpenFolderError] = useState<ErrorInfo | null>(null);

  const activeJob = useMemo(() => jobs.find((j) => j.id === activeJobId) ?? null, [jobs, activeJobId]);
  const canCancel = activeJob !== null && ACTIVE_STATES.has(activeJob.state);
  const canStartDownload = metadata !== null && outputDirectory.trim().length > 0 && !creating && !canCancel;

  const handleInspect = useCallback(async () => {
    setInspecting(true);
    setInspectError(null);
    setMetadata(null);
    setSelectedFormatId(null);
    try {
      const { metadata: result } = await coreClient.inspectDownloadUrl(url.trim());
      setMetadata(result);
    } catch (err) {
      setInspectError(asErrorInfo(err));
    } finally {
      setInspecting(false);
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
        ...(selectedFormatId ? { formatId: selectedFormatId } : {}),
      });
      setActiveJobId(jobId);
    } catch (err) {
      setCreateError(asErrorInfo(err));
    } finally {
      setCreating(false);
    }
  }, [url, outputDirectory, quality, selectedFormatId]);

  const handleCancel = useCallback(async () => {
    if (!activeJobId) return;
    setCancelBusy(true);
    try {
      await cancelJob(activeJobId);
    } catch (err) {
      setCreateError(asErrorInfo(err));
    } finally {
      setCancelBusy(false);
    }
  }, [activeJobId, cancelJob]);

  const handleOpenFolder = useCallback(async () => {
    const outputPath = activeJob?.result?.outputPath;
    if (typeof outputPath !== "string") return;
    setOpenFolderError(null);
    try {
      await coreClient.openContainingFolder(outputPath);
    } catch (err) {
      setOpenFolderError(asErrorInfo(err));
    }
  }, [activeJob]);

  const handleStartOver = useCallback(() => {
    setActiveJobId(null);
    setMetadata(null);
    setSelectedFormatId(null);
    setUrl("");
    setCreateError(null);
  }, []);

  // Distinct from handleStartOver: retrying a FAILED job should not drop the user back to
  // a blank paste screen, or make them re-inspect a URL that was already successfully
  // resolved -- only the download itself failed. See issue #56.
  const handleRetry = useCallback(() => {
    setActiveJobId(null);
    setCreateError(null);
  }, []);

  return (
    <div style={styles.page}>
      <h1 style={styles.h1}>Download</h1>
      <p style={styles.subtitle}>Paste a link to download it.</p>

      <section style={styles.section}>
        <div style={styles.row}>
          <input
            style={styles.input}
            type="text"
            placeholder="https://..."
            value={url}
            onChange={(e) => setUrl(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === "Enter" && !inspecting && url.trim() && !canCancel) void handleInspect();
            }}
            disabled={canCancel}
          />
          <button onClick={() => void handleInspect()} disabled={inspecting || !url.trim() || canCancel}>
            {inspecting ? "Inspecting..." : "Inspect"}
          </button>
        </div>
        <ErrorBanner error={inspectError} />

        {metadata ? (
          <div style={styles.card}>
            <div style={styles.metadataRow}>
              {metadata.thumbnailUrl ? (
                <img
                  src={metadata.thumbnailUrl}
                  alt=""
                  style={styles.thumbnail}
                  onError={(e) => {
                    (e.currentTarget as HTMLImageElement).style.display = "none";
                  }}
                />
              ) : null}
              <div>
                <div style={styles.title}>{metadata.title}</div>
                {metadata.uploader ? <div style={styles.muted}>{metadata.uploader}</div> : null}
                <div style={styles.muted}>Duration: {formatDuration(metadata.durationSeconds)}</div>
              </div>
            </div>
            {metadata.formats.length > 0 ? (
              // Backend already returns full per-format detail (codec, resolution, fps,
              // bitrate, size) -- this used to be discarded down to a bare count and shown
              // read-only. Clicking a row now picks that exact stream via formatId,
              // overriding the quality preset below for this download (issue #31).
              <details style={styles.formatsDisclosure}>
                <summary>
                  {metadata.formats.length} available format(s)
                  {selectedFormatId ? ` -- using ${selectedFormatId}` : ""}
                </summary>
                <ul style={styles.formatsList}>
                  {metadata.formats.map((format) => {
                    const isSelected = format.formatId === selectedFormatId;
                    return (
                      <li key={format.formatId}>
                        <button
                          type="button"
                          style={{ ...styles.formatOption, ...(isSelected ? styles.formatOptionSelected : {}) }}
                          aria-pressed={isSelected}
                          onClick={() => setSelectedFormatId(isSelected ? null : format.formatId)}
                        >
                          {format.formatId}
                          {format.resolution ? ` · ${format.resolution}` : ""}
                          {format.fps ? ` · ${format.fps}fps` : ""}
                          {format.hasVideo && format.videoCodec ? ` · ${format.videoCodec}` : ""}
                          {format.hasAudio && format.audioCodec ? ` · ${format.audioCodec}` : ""}
                          {!format.hasVideo && !format.hasAudio ? " · unknown" : ""}
                          {!format.hasVideo ? " · audio only" : !format.hasAudio ? " · video only" : ""}
                          {format.extension ? ` · .${format.extension}` : ""}
                          {formatBytes(format.filesizeBytes ?? format.approxFilesizeBytes) !== "?"
                            ? ` · ${formatBytes(format.filesizeBytes ?? format.approxFilesizeBytes)}`
                            : ""}
                        </button>
                      </li>
                    );
                  })}
                </ul>
              </details>
            ) : null}
          </div>
        ) : null}
      </section>

      {metadata ? (
        <section style={styles.section}>
          <h2 style={styles.h2}>Download options</h2>
          <PresetBar
            kind="DOWNLOAD"
            currentOptions={() => ({ quality, outputDirectory: outputDirectory.trim() })}
            onApply={(options) => {
              if (typeof options.quality === "string") setQuality(options.quality as QualityPreset);
              if (typeof options.outputDirectory === "string") setOutputDirectory(options.outputDirectory);
            }}
          />
          <div style={styles.row}>
            <label style={styles.label}>
              Quality:{" "}
              <select
                value={quality}
                onChange={(e) => setQuality(e.target.value as QualityPreset)}
                disabled={canCancel || selectedFormatId !== null}
              >
                {QUALITY_OPTIONS.map((preset) => (
                  <option key={preset} value={preset}>
                    {QUALITY_PRESET_LABELS[preset]}
                  </option>
                ))}
              </select>
            </label>
            {selectedFormatId ? (
              <span style={styles.muted}>
                {" "}
                Overridden by format {selectedFormatId} --{" "}
                <button type="button" onClick={() => setSelectedFormatId(null)}>
                  use quality preset instead
                </button>
              </span>
            ) : null}
          </div>
          <div style={styles.row}>
            <label style={styles.label}>
              Output folder:{" "}
              <input
                style={styles.input}
                type="text"
                placeholder="Choose an output folder"
                value={outputDirectory}
                onChange={(e) => setOutputDirectory(e.target.value)}
                disabled={canCancel}
              />
            </label>
          </div>
          <div style={styles.row}>
            <button onClick={() => void handleDownload()} disabled={!canStartDownload}>
              {creating ? "Starting..." : "Download"}
            </button>
            {canCancel ? (
              <button onClick={() => void handleCancel()} disabled={cancelBusy}>
                Cancel
              </button>
            ) : null}
          </div>
          <ErrorBanner error={createError} />
        </section>
      ) : null}

      {activeJob ? (
        <section style={styles.section}>
          <h2 style={styles.h2}>Job {activeJob.id}</h2>
          <div style={styles.card}>
            <div aria-live="polite">
              state: <strong>{activeJob.state}</strong>
            </div>
            <div aria-live="polite">{activeJob.progress.statusMessage}</div>
            {activeJob.progress.percentage !== undefined ? (
              <div
                style={styles.progressTrack}
                role="progressbar"
                aria-valuenow={Math.round(activeJob.progress.percentage)}
                aria-valuemin={0}
                aria-valuemax={100}
                aria-label="Download progress"
              >
                <div style={{ ...styles.progressFill, width: `${activeJob.progress.percentage}%` }} />
              </div>
            ) : null}
            <div style={styles.row}>
              <span>{formatBytes(activeJob.progress.processedBytes)} / {formatBytes(activeJob.progress.totalBytes)}</span>
              <span>{formatSpeedWithUnit(activeJob.progress.speedBytesPerSecond, speedUnit) ?? "?"}</span>
              <span>ETA {formatEta(activeJob.progress.etaSeconds)}</span>
            </div>

            {activeJob.state === "COMPLETED" ? (
              <div style={styles.okBanner} role="status">
                Download complete.
                {typeof activeJob.result?.outputPath === "string" ? (
                  <div style={styles.muted}>{activeJob.result.outputPath}</div>
                ) : null}
                <div style={styles.row}>
                  <button onClick={() => void handleOpenFolder()}>Open folder</button>
                  <button onClick={handleStartOver}>Download another</button>
                </div>
                <ErrorBanner error={openFolderError} />
              </div>
            ) : null}

            {activeJob.state === "FAILED" && activeJob.error ? (
              <div>
                <ErrorBanner error={activeJob.error} />
                <button onClick={handleRetry}>Try again</button>
              </div>
            ) : null}

            {activeJob.state === "CANCELLED" ? (
              <div>
                <p style={styles.muted}>Download cancelled.</p>
                <button onClick={handleStartOver}>Start over</button>
              </div>
            ) : null}
          </div>
        </section>
      ) : null}
    </div>
  );
}

const styles: Record<string, CSSProperties> = {
  page: {
    fontFamily: "system-ui, -apple-system, Segoe UI, sans-serif",
    maxWidth: 720,
    margin: "0 auto",
    padding: "1.5rem",
    display: "flex",
    flexDirection: "column",
    gap: "1rem",
  },
  h1: { fontSize: "1.5rem", margin: 0 },
  h2: { fontSize: "1.1rem", margin: "0 0 0.5rem 0" },
  subtitle: { color: "#555", marginTop: 0 },
  section: {
    border: "1px solid #ddd",
    borderRadius: 8,
    padding: "1rem",
    display: "flex",
    flexDirection: "column",
    gap: "0.5rem",
  },
  row: { display: "flex", gap: "0.75rem", alignItems: "center", flexWrap: "wrap" },
  label: { display: "flex", gap: "0.5rem", alignItems: "center" },
  input: { flex: "1 1 260px", padding: "0.4rem 0.5rem", fontSize: "0.9rem" },
  card: {
    border: "1px solid #eee",
    borderRadius: 6,
    padding: "0.75rem",
    background: "#fafafa",
    display: "flex",
    flexDirection: "column",
    gap: "0.5rem",
  },
  metadataRow: { display: "flex", gap: "0.75rem", alignItems: "flex-start" },
  thumbnail: { width: 120, borderRadius: 4 },
  formatsDisclosure: { marginTop: "0.5rem", fontSize: "0.85rem", color: "#666" },
  formatsList: {
    margin: "0.4rem 0 0",
    paddingLeft: 0,
    listStyle: "none",
    fontSize: "0.8rem",
    color: "#444",
    maxHeight: 220,
    overflowY: "auto",
  },
  formatOption: {
    display: "block",
    width: "100%",
    textAlign: "left",
    padding: "0.3rem 0.4rem",
    border: "1px solid transparent",
    borderRadius: 4,
    background: "none",
    font: "inherit",
    color: "inherit",
    cursor: "pointer",
  },
  formatOptionSelected: {
    border: "1px solid #3b82f6",
    background: "#eff6ff",
  },
  title: { fontWeight: 600, color: "#1a1a1a" },
  progressTrack: { background: "#e5e5e5", borderRadius: 4, height: 8, overflow: "hidden" },
  progressFill: { background: "#3b82f6", height: "100%" },
  errorBanner: {
    border: "1px solid #f5c2c7",
    background: "#f8d7da",
    color: "#842029",
    borderRadius: 6,
    padding: "0.5rem 0.75rem",
    fontSize: "0.9rem",
  },
  errorDetails: { fontSize: "0.8rem", marginTop: "0.25rem", whiteSpace: "pre-wrap" },
  okBanner: {
    border: "1px solid #badbcc",
    background: "#d1e7dd",
    color: "#0f5132",
    borderRadius: 6,
    padding: "0.5rem 0.75rem",
    fontSize: "0.9rem",
    display: "flex",
    flexDirection: "column",
    gap: "0.4rem",
  },
  // #888 on this page's #fafafa card background was ~2.9:1, below WCAG AA's 4.5:1 for
  // normal text (issue #32). #666 gives ~5.4:1.
  muted: { color: "#666", fontSize: "0.85rem" },
};
