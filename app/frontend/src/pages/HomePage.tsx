import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { open as openFilePicker } from "@tauri-apps/plugin-dialog";
import GlassCard from "../components/GlassCard";
import { useJobs } from "../hooks/useJobs";
import { useNavigation } from "../navigation/NavigationContext";
import * as coreClient from "../services/coreClient";
import type { DownloadMetadata, PlaylistInfo, QualityPreset } from "../types/download";
import { QUALITY_PRESET_LABELS } from "../types/download";
import type { JobSnapshot } from "../types/job";
import type { SpeedUnit } from "../types/settings";
import { asErrorInfo } from "../utils/errors";
import { formatSpeedWithUnit, formatTimestamp } from "../utils/format";
import { analyzePlaylistUrl, joinWindowsPath } from "../utils/playlistUrl";
import styles from "./HomePage.module.css";

const QUALITY_OPTIONS: QualityPreset[] = ["BEST", "2160P", "1440P", "1080P", "720P", "480P", "AUDIO_ONLY"];
const ACTIVE_STATES = new Set(["QUEUED", "STARTING", "RUNNING", "PAUSED"]);
const PLAYLIST_NOT_SUPPORTED = "E_PLAYLIST_NOT_SUPPORTED";

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

function ErrorBanner({ error }: { error: any }) {
  if (!error) return null;
  return (
    <div style={{ border: "1px solid #f5c2c7", background: "#f8d7da", color: "#842029", borderRadius: 6, padding: "0.5rem 0.75rem", fontSize: "0.9rem" }} role="alert">
      <strong>{error.category}</strong> ({error.code}): {error.message}
    </div>
  );
}

export default function HomePage() {
  const { navigate } = useNavigation();
  const { jobs, cancelJob } = useJobs();
  const [url, setUrl] = useState("");
  const [history, setHistory] = useState<JobSnapshot[]>([]);
  const [inspecting, setInspecting] = useState(false);
  const [inspectError, setInspectError] = useState<any>(null);
  const [metadata, setMetadata] = useState<DownloadMetadata | null>(null);
  const [selectedFormatId, setSelectedFormatId] = useState<string | null>(null);
  const [quality, setQuality] = useState<QualityPreset>("BEST");
  const [outputDirectory, setOutputDirectory] = useState("");
  const [speedUnit, setSpeedUnit] = useState<SpeedUnit>("MBps");
  const [playlist, setPlaylist] = useState<PlaylistInfo | null>(null);
  const [playlistLoading, setPlaylistLoading] = useState(false);
  const [comboChoiceUrl, setComboChoiceUrl] = useState<string | null>(null);
  const [playlistFolder, setPlaylistFolder] = useState("");
  const [playlistJobIds, setPlaylistJobIds] = useState<string[]>([]);
  const [creating, setCreating] = useState(false);
  const [createError, setCreateError] = useState<any>(null);
  const [activeJobId, setActiveJobId] = useState<string | null>(null);
  const [cancelBusy, setCancelBusy] = useState(false);

  useEffect(() => {
    coreClient
      .getSettings()
      .then(({ settings }) => {
        setOutputDirectory(settings.general.defaultOutputDirectory);
        const upper = settings.downloads.defaultQuality.toUpperCase() as QualityPreset;
        if (QUALITY_OPTIONS.includes(upper)) setQuality(upper);
        setSpeedUnit(settings.downloads.speedUnits);
      })
      .catch(() => {});
  }, []);

  useEffect(() => {
    coreClient
      .listJobHistory(5)
      .then(({ jobs: entries }) => setHistory(entries))
      .catch(() => {});
    const unsubscribe = coreClient.subscribeToJobEvents((event) => {
      if (event.event === "jobCompleted" || event.event === "jobFailed" || event.event === "jobCancelled") {
        coreClient
          .listJobHistory(5)
          .then(({ jobs: entries }) => setHistory(entries))
          .catch(() => {});
      }
    });
    return unsubscribe;
  }, []);

  const activeJob = useMemo(() => jobs.find((j) => j.id === activeJobId) ?? null, [jobs, activeJobId]);
  const canCancel = activeJob !== null && ACTIVE_STATES.has(activeJob.state as any);
  const canStartDownload = metadata !== null && outputDirectory.trim().length > 0 && !creating && !canCancel;

  const loadPlaylist = useCallback(
    async (target: string) => {
      setPlaylistLoading(true);
      setInspectError(null);
      setMetadata(null);
      setPlaylist(null);
      try {
        const { playlist: result } = await coreClient.inspectPlaylistUrl(target);
        setPlaylist(result);
        try {
          const { name } = await coreClient.suggestPlaylistFolder(outputDirectory.trim());
          setPlaylistFolder(name);
        } catch {
          setPlaylistFolder(result.title);
        }
      } catch (err) {
        setInspectError(asErrorInfo(err));
      } finally {
        setPlaylistLoading(false);
      }
    },
    [outputDirectory],
  );

  const handleInspect = useCallback(
    async (urlOverride?: string) => {
      const target = (urlOverride ?? url).trim();
      if (!target) return;
      setInspecting(true);
      setInspectError(null);
      setMetadata(null);
      setPlaylist(null);
      setComboChoiceUrl(null);
      setSelectedFormatId(null);
      try {
        const { metadata: result } = await coreClient.inspectDownloadUrl(target);
        setMetadata(result);
        if (analyzePlaylistUrl(target).hasPlaylist) {
          setComboChoiceUrl(target);
        }
      } catch (err) {
        const info = asErrorInfo(err);
        if (info?.code === PLAYLIST_NOT_SUPPORTED) {
          setInspecting(false);
          await loadPlaylist(target);
          return;
        }
        setInspectError(info);
      } finally {
        setInspecting(false);
      }
    },
    [url, loadPlaylist],
  );

  const handlePasteUrl = useCallback(
    (e: React.ClipboardEvent<HTMLInputElement>) => {
      if (canCancel || inspecting) return;
      const pasted = e.clipboardData.getData("text").trim();
      if (!/^https?:\/\/\S+$/i.test(pasted)) return;
      e.preventDefault();
      setUrl(pasted);
      void handleInspect(pasted);
    },
    [canCancel, inspecting, handleInspect],
  );

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

  const handleDownloadPlaylist = useCallback(async () => {
    if (!playlist || playlist.entries.length === 0) return;
    const folder = playlistFolder.trim();
    if (!folder) return;

    setCreating(true);
    setCreateError(null);
    const destination = joinWindowsPath(outputDirectory.trim(), folder);
    const created: string[] = [];
    try {
      for (const entry of playlist.entries) {
        const { jobId } = await coreClient.createDownloadJob({
          url: entry.url,
          outputDirectory: destination,
          quality,
          playlistIndex: entry.index,
          playlistCount: playlist.entries.length,
          ...(created.length > 0 ? { runAfter: [created[created.length - 1]] } : {}),
        });
        created.push(jobId);
      }
      setPlaylistJobIds(created);
    } catch (err) {
      setPlaylistJobIds(created);
      setCreateError(asErrorInfo(err));
    } finally {
      setCreating(false);
    }
  }, [playlist, playlistFolder, outputDirectory, quality]);

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

  const handleStartOver = useCallback(() => {
    setActiveJobId(null);
    setMetadata(null);
    setSelectedFormatId(null);
    setUrl("");
    setCreateError(null);
    setPlaylist(null);
    setPlaylistJobIds([]);
    setPlaylistFolder("");
    setComboChoiceUrl(null);
    setInspectError(null);
  }, []);

  const handleRetry = useCallback(() => {
    setActiveJobId(null);
    setCreateError(null);
  }, []);

  const playlistProgress = useMemo(() => {
    if (playlistJobIds.length === 0) return null;
    const mine = jobs.filter((j) => playlistJobIds.includes(j.id));
    return {
      total: playlistJobIds.length,
      completed: mine.filter((j) => j.state === "COMPLETED").length,
      failed: mine.filter((j) => j.state === "FAILED").length,
      running: mine.filter((j) => j.state === "RUNNING" || j.state === "STARTING").length,
    };
  }, [jobs, playlistJobIds]);

  const jobSectionRef = useRef<HTMLElement>(null);
  const failureRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (activeJobId) jobSectionRef.current?.focus();
  }, [activeJobId]);

  useEffect(() => {
    if (activeJob?.state === "FAILED") failureRef.current?.focus();
  }, [activeJob?.state]);

  const handleConvertDrop = (paths: string[]) => {
    navigate({ kind: "convert", prefillFilePath: paths[0], mode: "convert" });
  };

  const handleBrowseConvertFile = useCallback(async () => {
    const selected = await openFilePicker({
      multiple: false,
      title: "Choose a file to convert or compress",
    });
    if (typeof selected === "string") {
      navigate({ kind: "convert", prefillFilePath: selected, mode: "convert" });
    }
  }, [navigate]);

  const showDownloadUI = (metadata !== null || playlist !== null || activeJobId !== null || inspectError !== null) && (url.length > 0 || activeJobId !== null);

  return (
    <div className={styles.wrap}>
      <div className={styles.heading}>
        <h1 className={styles.title}>What are we doing today?</h1>
        <p className={styles.subtitle}>Drag a file in, or pick a card to get started.</p>
      </div>
      <div className={styles.mainContainer}>
        <div className={styles.cardsColumn}>
        <GlassCard className={`${styles.box} ${styles.downloadCard}`} ariaLabel="Download from a URL">
          <div className={styles.icon}>&#8595;</div>
          <h2 className={styles.boxTitle}>Download from any link</h2>
          <p className={styles.boxSubtitle}>Paste a link. You can paste links to YT playlists, TikToks, Streamable Videos, Medal clips + more</p>
          <div className={styles.urlInputContainer}>
            <input
              className={styles.urlInput}
              type="text"
              placeholder="Paste a URL..."
              value={url}
              onChange={(e) => setUrl(e.target.value)}
              onPaste={handlePasteUrl}
              onKeyDown={(e) => {
                if (e.key === "Enter" && !inspecting && url.trim() && !canCancel) void handleInspect();
              }}
              disabled={canCancel}
            />
            <button
              className={styles.downloadButton}
              onClick={() => void handleInspect()}
              disabled={inspecting || !url.trim() || canCancel}
            >
              {inspecting ? "Inspecting..." : "Inspect"}
            </button>
          </div>
          {showDownloadUI && (
            <div className={styles.downloadDetails}>
              {playlistLoading && <p style={{ color: "var(--color-text-secondary)", fontSize: "0.85rem" }}>Reading playlist...</p>}
              <ErrorBanner error={inspectError} />

              {metadata && (
                <div style={{ border: "1px solid var(--color-surface-border)", borderRadius: 8, padding: "0.75rem", background: "rgba(99, 102, 241, 0.05)" }}>
                  <div style={{ display: "flex", gap: "0.75rem", alignItems: "flex-start" }}>
                    {metadata.thumbnailUrl && (
                      <img
                        src={metadata.thumbnailUrl}
                        alt=""
                        style={{ width: 80, borderRadius: 4 }}
                        onError={(e) => {
                          (e.currentTarget as HTMLImageElement).style.display = "none";
                        }}
                      />
                    )}
                    <div style={{ minWidth: 0 }}>
                      <div style={{ fontWeight: 600, color: "var(--color-text-primary)", overflowWrap: "break-word" }}>{metadata.title}</div>
                      {metadata.uploader && <div style={{ color: "var(--color-text-secondary)", fontSize: "0.85rem" }}>{metadata.uploader}</div>}
                      <div style={{ color: "var(--color-text-secondary)", fontSize: "0.85rem" }}>Duration: {formatDuration(metadata.durationSeconds)}</div>
                    </div>
                  </div>
                </div>
              )}

              {metadata && (
                <div>
                  <div style={{ display: "flex", gap: "0.75rem", alignItems: "center", flexWrap: "wrap", fontSize: "0.9rem" }}>
                    <label style={{ display: "flex", gap: "0.5rem", alignItems: "center" }}>
                      Quality:{" "}
                      <select
                        value={quality}
                        onChange={(e) => setQuality(e.target.value as QualityPreset)}
                        disabled={canCancel || selectedFormatId !== null}
                        style={{ padding: "0.4rem 0.5rem", fontSize: "0.9rem" }}
                      >
                        {QUALITY_OPTIONS.map((preset) => (
                          <option key={preset} value={preset}>
                            {QUALITY_PRESET_LABELS[preset]}
                          </option>
                        ))}
                      </select>
                    </label>
                  </div>
                  <div style={{ display: "flex", gap: "0.75rem", alignItems: "center", marginTop: "0.5rem" }}>
                    <label style={{ display: "flex", gap: "0.5rem", alignItems: "center", fontSize: "0.9rem", flex: "1 1 260px" }}>
                      Output folder:{" "}
                      <input
                        style={{ flex: "1 1 260px", padding: "0.4rem 0.5rem", fontSize: "0.9rem" }}
                        type="text"
                        placeholder="Choose an output folder"
                        value={outputDirectory}
                        onChange={(e) => setOutputDirectory(e.target.value)}
                        disabled={canCancel}
                      />
                    </label>
                  </div>
                  <div style={{ display: "flex", gap: "0.75rem", marginTop: "0.75rem" }}>
                    <button
                      onClick={() => void handleDownload()}
                      disabled={!canStartDownload}
                      style={{ padding: "0.5rem 1rem", fontSize: "0.9rem", background: "var(--color-accent)", color: "white", border: "none", borderRadius: 6, cursor: canStartDownload ? "pointer" : "not-allowed", opacity: canStartDownload ? 1 : 0.5 }}
                    >
                      {creating ? "Starting..." : "Download"}
                    </button>
                    {canCancel && (
                      <button
                        onClick={() => void handleCancel()}
                        disabled={cancelBusy}
                        style={{ padding: "0.5rem 1rem", fontSize: "0.9rem", background: "#dc3545", color: "white", border: "none", borderRadius: 6, cursor: "pointer", opacity: cancelBusy ? 0.5 : 1 }}
                      >
                        Cancel
                      </button>
                    )}
                  </div>
                  <ErrorBanner error={createError} />
                </div>
              )}

              {activeJob && (
                <div style={{ border: "1px solid var(--color-surface-border)", borderRadius: 8, padding: "0.75rem", background: "rgba(99, 102, 241, 0.05)", marginTop: "0.75rem" }} ref={jobSectionRef as any} tabIndex={-1}>
                  <div style={{ marginBottom: "0.5rem" }}>
                    <strong>State:</strong> {activeJob.state}
                  </div>
                  <div style={{ marginBottom: "0.5rem", color: "var(--color-text-secondary)" }}>{activeJob.progress.statusMessage}</div>
                  {activeJob.progress.percentage !== undefined && (
                    <div style={{ background: "rgba(99, 102, 241, 0.2)", borderRadius: 4, height: 6, overflow: "hidden", marginBottom: "0.5rem" }}>
                      <div style={{ background: "var(--color-accent)", height: "100%", width: `${activeJob.progress.percentage}%`, transition: "width 0.3s" }} />
                    </div>
                  )}
                  <div style={{ display: "flex", gap: "1rem", fontSize: "0.85rem", color: "var(--color-text-secondary)" }}>
                    <span>{formatBytes(activeJob.progress.processedBytes)} / {formatBytes(activeJob.progress.totalBytes)}</span>
                    <span>{formatSpeedWithUnit(activeJob.progress.speedBytesPerSecond, speedUnit) ?? "?"}</span>
                    <span>ETA {formatEta(activeJob.progress.etaSeconds)}</span>
                  </div>

                  {activeJob.state === "COMPLETED" && (
                    <div style={{ marginTop: "0.75rem", borderTop: "1px solid rgba(99, 102, 241, 0.2)", paddingTop: "0.5rem" }}>
                      <p style={{ color: "var(--color-text-secondary)", fontSize: "0.85rem", margin: "0.25rem 0" }}>Download complete.</p>
                      {typeof activeJob.result?.outputPath === "string" && (
                        <p style={{ color: "var(--color-text-secondary)", fontSize: "0.85rem", wordBreak: "break-word", margin: "0.25rem 0" }}>{activeJob.result.outputPath}</p>
                      )}
                      <button onClick={handleStartOver} style={{ marginTop: "0.5rem", padding: "0.4rem 0.8rem", fontSize: "0.85rem", background: "var(--color-accent)", color: "white", border: "none", borderRadius: 4, cursor: "pointer" }}>
                        Download another
                      </button>
                    </div>
                  )}

                  {activeJob.state === "FAILED" && activeJob.error && (
                    <div style={{ marginTop: "0.75rem", borderTop: "1px solid rgba(99, 102, 241, 0.2)", paddingTop: "0.5rem" }} ref={failureRef} tabIndex={-1}>
                      <ErrorBanner error={activeJob.error} />
                      <button onClick={handleRetry} style={{ marginTop: "0.5rem", padding: "0.4rem 0.8rem", fontSize: "0.85rem", background: "var(--color-accent)", color: "white", border: "none", borderRadius: 4, cursor: "pointer" }}>
                        Try again
                      </button>
                    </div>
                  )}

                  {activeJob.state === "CANCELLED" && (
                    <div style={{ marginTop: "0.75rem", borderTop: "1px solid rgba(99, 102, 241, 0.2)", paddingTop: "0.5rem" }}>
                      <p style={{ color: "var(--color-text-secondary)", fontSize: "0.85rem", margin: "0.25rem 0" }}>Download cancelled.</p>
                      <button onClick={handleStartOver} style={{ marginTop: "0.5rem", padding: "0.4rem 0.8rem", fontSize: "0.85rem", background: "var(--color-accent)", color: "white", border: "none", borderRadius: 4, cursor: "pointer" }}>
                        Start over
                      </button>
                    </div>
                  )}
                </div>
              )}
            </div>
          )}
        </GlassCard>
        <GlassCard
          className={styles.box}
          onFilesDropped={handleConvertDrop}
          ariaLabel="Convert or compress a local file"
        >
          <div className={styles.icon}>&#8646;</div>
          <h2 className={styles.boxTitle}>Convert &amp; Compress</h2>
          <div className={styles.dropzoneContainer}>
            <div className={styles.dropzoneIcon}>&#8646;</div>
            <p className={styles.dropzoneText}>Drop a file here to get started.</p>
            <button
              className={styles.dropzoneLink}
              onClick={(e) => {
                e.stopPropagation();
                void handleBrowseConvertFile();
              }}
            >
              or choose a file...
            </button>
          </div>
        </GlassCard>
        </div>
        {history.length > 0 && (
          <div className={styles.historyContainer}>
          <div className={styles.historyTitle}>Recent</div>
          <div className={styles.historyList}>
            {history.map((job) => {
              const typeLabel = job.type === "CONVERSION" ? "Convert" : job.type === "COMPRESSION" ? "Compress" : job.type === "DOWNLOAD" ? "Download" : job.type;
              const statusLabel = job.state === "COMPLETED" ? "Completed" : job.state === "CANCELLED" ? "Cancelled" : "Failed";
              const outputPath = typeof job.result?.outputPath === "string" ? (job.result.outputPath as string) : undefined;
              const timestamp = formatTimestamp(job.completedAt ?? job.createdAt);

              return (
                <div key={job.id} className={styles.historyItem}>
                  <div className={styles.historyItemHeader}>
                    <span className={styles.historyTypeBadge}>{typeLabel}</span>
                    <span className={styles.historyItemStatus}>{statusLabel}</span>
                  </div>
                  <div className={styles.historyItemDetail}>{timestamp}</div>
                  {outputPath && <div className={styles.historyItemDetail}>{outputPath}</div>}
                  {job.error && <div className={styles.historyItemDetail} style={{ color: "var(--color-error)" }}>{job.error.message}</div>}
                </div>
              );
            })}
          </div>
          </div>
        )}
      </div>
    </div>
  );
}
