import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from "react";
import { open as openFilePicker } from "@tauri-apps/plugin-dialog";
import GlassCard from "../components/GlassCard";
import { useJobs } from "../hooks/useJobs";
import { useNavigation } from "../navigation/NavigationContext";
import * as coreClient from "../services/coreClient";
import type { DownloadMetadata, PlaylistInfo, QualityPreset } from "../types/download";
import { QUALITY_PRESET_LABELS } from "../types/download";
import type { ErrorInfo } from "../types/error";
import type { JobSnapshot } from "../types/job";
import type { SpeedUnit } from "../types/settings";
import { asErrorInfo } from "../utils/errors";
import { formatSpeedWithUnit, formatTimestamp } from "../utils/format";
import { analyzePlaylistUrl, joinWindowsPath, withoutPlaylistParam } from "../utils/playlistUrl";
import styles from "./HomePage.module.css";

const QUALITY_OPTIONS: QualityPreset[] = ["BEST", "2160P", "1440P", "1080P", "720P", "480P", "AUDIO_ONLY"];

// The download card's inputs and selects, themed. Without this they render as the
// browser's default light controls on a dark surface -- white boxes with black text in the
// middle of the app's own palette. Mirrors .urlInput in HomePage.module.css, which is the
// one control on this card that was already themed.
const CONTROL_STYLE: React.CSSProperties = {
  padding: "0.4rem 0.5rem",
  fontSize: "0.9rem",
  border: "1px solid var(--color-surface-border)",
  borderRadius: 8,
  background: "var(--color-surface)",
  color: "var(--color-text-primary)",
  fontFamily: "inherit",
};
const FIELD_STYLE: React.CSSProperties = { ...CONTROL_STYLE, flex: "1 1 260px" };
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

interface CardBox {
  left: number;
  top: number;
  width: number;
  height: number;
}

function measureCard(el: HTMLElement): CardBox {
  // offset* rather than getBoundingClientRect(): these ignore CSS transforms, so a
  // measurement taken while a previous FLIP is still settling isn't poisoned by it.
  return { left: el.offsetLeft, top: el.offsetTop, width: el.offsetWidth, height: el.offsetHeight };
}

// Animates the home cards between the side-by-side and stacked layouts.
//
// A plain CSS transition can't do this: the change is a flex reflow (the Convert card
// moves from *beside* the download card to *below* it), and neither `flex-direction` nor
// a reflow-driven position change is an animatable property -- which is why adding
// `transition` here appeared to do nothing. So this is a FLIP: record where each card sat
// (First), let React commit the new layout (Last), then start each card off at its old
// geometry (Invert) and animate it to the new one (Play).
//
// `layoutKey` is what identifies a layout, not a render. The download card re-renders
// constantly while a job reports progress, and those renders must not restart the
// animation -- only a change of key does.
function useCardLayoutFlip(containerRef: React.RefObject<HTMLDivElement | null>, layoutKey: string) {
  const previousBoxes = useRef<CardBox[] | null>(null);
  const previousKey = useRef<string | null>(null);

  // Deliberately no dependency array: the boxes are re-measured after every commit so the
  // "First" geometry is current when a layout change does land (a card that grew as
  // metadata loaded has its real height recorded, not a stale one).
  useLayoutEffect(() => {
    const container = containerRef.current;
    if (!container) return;

    // The Web Animations API is decorative here, and a throw inside a layout effect
    // unmounts the whole tree -- so an environment without it must lose the animation, not
    // the page. (jsdom is one such environment, which is also why this page could not be
    // rendered in a test at all before.)
    if (typeof Element.prototype.getAnimations !== "function" || typeof Element.prototype.animate !== "function") {
      return;
    }

    const cards = Array.from(container.children) as HTMLElement[];
    const layoutChanged = previousKey.current !== null && previousKey.current !== layoutKey;
    const animating = cards.some((card) => card.getAnimations().some((a) => a.playState === "running"));

    // Mid-flight geometry is the animation's, not the layout's -- sampling it would leave
    // a bogus "First" for the next transition. Skip, unless the layout just changed, in
    // which case cancelling first restores the true resting geometry to measure.
    if (animating && !layoutChanged) return;
    if (layoutChanged) cards.forEach((card) => card.getAnimations().forEach((a) => a.cancel()));

    const from = previousBoxes.current;
    const to = cards.map(measureCard);
    previousBoxes.current = to;
    previousKey.current = layoutKey;

    if (!layoutChanged || !from || from.length !== to.length) return;
    if (window.matchMedia("(prefers-reduced-motion: reduce)").matches) return;

    cards.forEach((card, i) => {
      const a = from[i];
      const b = to[i];
      const dx = a.left - b.left;
      const dy = a.top - b.top;
      if (dx === 0 && dy === 0 && a.width === b.width && a.height === b.height) return;
      card.animate(
        [
          { transform: `translate(${dx}px, ${dy}px)`, width: `${a.width}px`, height: `${a.height}px` },
          { transform: "translate(0px, 0px)", width: `${b.width}px`, height: `${b.height}px` },
        ],
        { duration: 420, easing: "cubic-bezier(0.4, 0, 0.2, 1)" }
      );
    });
  });
}

// Same treatment DownloaderPage gives errors, rather than the raw category/code dump this
// page had: a connectivity failure is the one class a user can usually just retry (issue
// #55), and the raw diagnostic text -- which can be a whole Python traceback, see
// downloader.py's emit_error -- belongs behind a disclosure, not permanently on screen
// (issue #33). Colours come from the theme tokens; the hardcoded light-mode red this used
// was unreadable on Gravity's dark surface.
function ErrorBanner({ error }: { error: ErrorInfo | null | undefined }) {
  if (!error) return null;
  const box = {
    border: "1px solid var(--color-error)",
    background: "rgba(220, 53, 69, 0.12)",
    color: "var(--color-text-primary)",
    borderRadius: 6,
    padding: "0.5rem 0.75rem",
    fontSize: "0.9rem",
  } as const;

  if (error.category === "NETWORK_ERROR") {
    return (
      <div style={box} role="alert">
        Can&apos;t reach the network. Check your internet connection and try again.
      </div>
    );
  }
  return (
    <div style={box} role="alert">
      <strong>{error.category}</strong> ({error.code}): {error.message}
      {error.details && error.details !== error.message ? (
        <details style={{ marginTop: "0.35rem", fontSize: "0.85rem", color: "var(--color-text-secondary)" }}>
          <summary>Technical details</summary>
          <div style={{ overflowWrap: "break-word", wordBreak: "break-word" }}>{error.details}</div>
        </details>
      ) : null}
    </div>
  );
}

export default function HomePage() {
  const { navigate } = useNavigation();
  const { jobs, cancelJob } = useJobs();
  const [url, setUrl] = useState("");
  const [history, setHistory] = useState<JobSnapshot[]>([]);
  const [inspecting, setInspecting] = useState(false);
  const [inspectError, setInspectError] = useState<ErrorInfo | null>(null);
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
  const [createError, setCreateError] = useState<ErrorInfo | null>(null);
  const [activeJobId, setActiveJobId] = useState<string | null>(null);
  const [cancelBusy, setCancelBusy] = useState(false);
  const [openFolderError, setOpenFolderError] = useState<ErrorInfo | null>(null);

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
    setOpenFolderError(null);
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
  const openFolderButtonRef = useRef<HTMLButtonElement>(null);

  useEffect(() => {
    if (activeJobId) jobSectionRef.current?.focus();
  }, [activeJobId]);

  useEffect(() => {
    if (activeJob?.state === "FAILED") failureRef.current?.focus();
    if (activeJob?.state === "COMPLETED") openFolderButtonRef.current?.focus();
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

  // playlistLoading belongs here too: the "Reading playlist..." message lives inside this
  // block, so without it the one slow step in the playlist flow (enumerating the list,
  // which is a network round-trip) showed the user nothing at all.
  const showDownloadUI =
    (metadata !== null || playlist !== null || playlistLoading || activeJobId !== null || inspectError !== null) &&
    (url.length > 0 || activeJobId !== null);
  const downloadInputActive = url.length > 0;

  const boxesRef = useRef<HTMLDivElement>(null);
  // Both flags change the cards' geometry, so either one is a new layout to animate to.
  useCardLayoutFlip(boxesRef, `${downloadInputActive}:${showDownloadUI}`);

  return (
    <div className={styles.wrap}>
      <div className={styles.heading}>
        <h1 className={styles.title}>What are we doing today?</h1>
        <p className={styles.subtitle}>Drag a file in, or pick a card to get started.</p>
      </div>
      <div ref={boxesRef} className={`${styles.boxes} ${downloadInputActive ? styles.boxesExpanded : ""}`}>
        <GlassCard className={`${styles.box} ${styles.downloadCard}`} ariaLabel="Download from a URL">
          <div className={styles.icon}>&#8595;</div>
          <h2 className={styles.boxTitle}>Download from any link</h2>
          {!downloadInputActive && (
            <p className={styles.boxSubtitle}>Paste a link. You can paste links to YT playlists, TikToks, Streamable Videos, Medal clips + more</p>
          )}
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

              {/* The "shared from a playlist" fork. `comboChoiceUrl` was already being set
                  here but nothing rendered it, so a watch?v=X&list=Y link silently
                  resolved to just the one video with no way to ask for the list. */}
              {comboChoiceUrl && (
                <div
                  style={{ border: "1px solid var(--color-surface-border)", borderRadius: 8, padding: "0.75rem", display: "flex", flexDirection: "column", gap: "0.5rem", fontSize: "0.9rem" }}
                  role="group"
                  aria-label="This link is part of a playlist"
                >
                  <div>This link is part of a playlist.</div>
                  <div style={{ display: "flex", gap: "0.75rem", flexWrap: "wrap" }}>
                    <button
                      type="button"
                      onClick={() => {
                        setComboChoiceUrl(null);
                        setUrl(withoutPlaylistParam(comboChoiceUrl));
                      }}
                    >
                      Just this video
                    </button>
                    <button
                      type="button"
                      onClick={() => {
                        const target = comboChoiceUrl;
                        setComboChoiceUrl(null);
                        void loadPlaylist(target);
                      }}
                    >
                      The whole playlist
                    </button>
                  </div>
                </div>
              )}

              {/* Per-stream selection (issue #31). `selectedFormatId` is read by
                  handleDownload and disables the quality preset, but with no list rendered
                  it could never become non-null -- the whole feature was unreachable here. */}
              {metadata && metadata.formats.length > 0 && (
                <details style={{ fontSize: "0.85rem", color: "var(--color-text-secondary)" }}>
                  <summary>
                    {metadata.formats.length} available format(s)
                    {selectedFormatId ? ` -- using ${selectedFormatId}` : ""}
                  </summary>
                  <ul style={{ margin: "0.4rem 0 0", paddingLeft: 0, listStyle: "none", maxHeight: 220, overflowY: "auto" }}>
                    {metadata.formats.map((format) => {
                      const isSelected = format.formatId === selectedFormatId;
                      return (
                        <li key={format.formatId}>
                          <button
                            type="button"
                            aria-pressed={isSelected}
                            onClick={() => setSelectedFormatId(isSelected ? null : format.formatId)}
                            style={{
                              display: "block",
                              width: "100%",
                              textAlign: "left",
                              padding: "0.25rem 0.4rem",
                              fontSize: "0.8rem",
                              cursor: "pointer",
                              background: isSelected ? "rgba(99, 102, 241, 0.25)" : "transparent",
                              color: "var(--color-text-primary)",
                              border: "1px solid transparent",
                              borderRadius: 4,
                            }}
                          >
                            {format.formatId}
                            {format.resolution ? ` \u00b7 ${format.resolution}` : ""}
                            {format.fps ? ` \u00b7 ${format.fps}fps` : ""}
                            {format.hasVideo && format.videoCodec ? ` \u00b7 ${format.videoCodec}` : ""}
                            {format.hasAudio && format.audioCodec ? ` \u00b7 ${format.audioCodec}` : ""}
                            {!format.hasVideo ? " \u00b7 audio only" : !format.hasAudio ? " \u00b7 video only" : ""}
                            {format.extension ? ` \u00b7 .${format.extension}` : ""}
                            {formatBytes(format.filesizeBytes ?? format.approxFilesizeBytes) !== "?"
                              ? ` \u00b7 ${formatBytes(format.filesizeBytes ?? format.approxFilesizeBytes)}`
                              : ""}
                          </button>
                        </li>
                      );
                    })}
                  </ul>
                </details>
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
                        style={CONTROL_STYLE}
                      >
                        {QUALITY_OPTIONS.map((preset) => (
                          <option key={preset} value={preset}>
                            {QUALITY_PRESET_LABELS[preset]}
                          </option>
                        ))}
                      </select>
                    </label>
                    {selectedFormatId && (
                      <span style={{ color: "var(--color-text-secondary)", fontSize: "0.85rem" }}>
                        Overridden by format {selectedFormatId} --{" "}
                        <button type="button" onClick={() => setSelectedFormatId(null)}>
                          use quality preset instead
                        </button>
                      </span>
                    )}
                  </div>
                  <div style={{ display: "flex", gap: "0.75rem", alignItems: "center", marginTop: "0.5rem" }}>
                    <label style={{ display: "flex", gap: "0.5rem", alignItems: "center", fontSize: "0.9rem", flex: "1 1 260px" }}>
                      Output folder:{" "}
                      <input
                        style={FIELD_STYLE}
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

              {/* Playlist fan-out (issue #41). `playlist`, `playlistFolder`,
                  `handleDownloadPlaylist` and `playlistProgress` were all present in this
                  file already -- nothing rendered any of them, so pasting a playlist link
                  enumerated it successfully and then showed the user an empty card. */}
              {playlist && (
                <div style={{ border: "1px solid var(--color-surface-border)", borderRadius: 8, padding: "0.75rem", display: "flex", flexDirection: "column", gap: "0.5rem" }}>
                  <div style={{ fontWeight: 600, color: "var(--color-text-primary)", overflowWrap: "break-word" }}>{playlist.title}</div>
                  <div style={{ color: "var(--color-text-secondary)", fontSize: "0.85rem" }}>
                    {playlist.count} video{playlist.count === 1 ? "" : "s"}
                    {playlist.uploader ? ` \u00b7 ${playlist.uploader}` : ""}
                    {" \u00b7 downloaded one at a time, in order."}
                  </div>
                  {playlist.truncated && (
                    <div style={{ border: "1px solid var(--color-surface-border)", borderRadius: 6, padding: "0.5rem 0.75rem", fontSize: "0.85rem" }} role="status">
                      This playlist is longer than the {playlist.count}-video limit. Only the
                      first {playlist.count} will be downloaded.
                    </div>
                  )}

                  <details style={{ fontSize: "0.85rem", color: "var(--color-text-secondary)" }}>
                    <summary>Show the {playlist.count} videos</summary>
                    <ol style={{ margin: "0.4rem 0 0", paddingLeft: "1.4rem", maxHeight: 220, overflowY: "auto" }}>
                      {playlist.entries.map((entry) => (
                        <li key={`${entry.index}-${entry.url}`} style={{ overflowWrap: "break-word", wordBreak: "break-word" }}>
                          {entry.title}
                          {entry.durationSeconds !== undefined ? ` \u00b7 ${formatDuration(entry.durationSeconds)}` : ""}
                        </li>
                      ))}
                    </ol>
                  </details>

                  <label style={{ display: "flex", gap: "0.5rem", alignItems: "center", fontSize: "0.9rem", flexWrap: "wrap" }}>
                    Quality for all videos:{" "}
                    <select
                      value={quality}
                      onChange={(e) => setQuality(e.target.value as QualityPreset)}
                      disabled={creating}
                      style={CONTROL_STYLE}
                    >
                      {QUALITY_OPTIONS.map((preset) => (
                        <option key={preset} value={preset}>
                          {QUALITY_PRESET_LABELS[preset]}
                        </option>
                      ))}
                    </select>
                  </label>
                  <label style={{ display: "flex", gap: "0.5rem", alignItems: "center", fontSize: "0.9rem" }}>
                    Output folder:{" "}
                    <input
                      style={FIELD_STYLE}
                      type="text"
                      placeholder="Choose an output folder"
                      value={outputDirectory}
                      onChange={(e) => setOutputDirectory(e.target.value)}
                      disabled={creating}
                    />
                  </label>
                  <label style={{ display: "flex", gap: "0.5rem", alignItems: "center", fontSize: "0.9rem" }}>
                    Playlist folder name:{" "}
                    <input
                      style={FIELD_STYLE}
                      type="text"
                      placeholder="playlist #1"
                      value={playlistFolder}
                      onChange={(e) => setPlaylistFolder(e.target.value)}
                      disabled={creating}
                    />
                  </label>
                  {/* Two editable fields combine into one destination, and "where did my 47
                      files go" is otherwise only answerable after the fact. */}
                  {outputDirectory.trim() && playlistFolder.trim() ? (
                    <p style={{ color: "var(--color-text-secondary)", fontSize: "0.85rem", overflowWrap: "break-word", wordBreak: "break-word", margin: 0 }}>
                      Files go to {joinWindowsPath(outputDirectory.trim(), playlistFolder.trim())}, named{" "}
                      <code>01 - Title</code>, <code>02 - Title</code>, ...
                    </p>
                  ) : null}

                  <div>
                    <button
                      onClick={() => void handleDownloadPlaylist()}
                      disabled={
                        creating ||
                        playlist.entries.length === 0 ||
                        !outputDirectory.trim() ||
                        !playlistFolder.trim() ||
                        playlistJobIds.length > 0
                      }
                      style={{ padding: "0.5rem 1rem", fontSize: "0.9rem", background: "var(--color-accent)", color: "white", border: "none", borderRadius: 6, cursor: "pointer" }}
                    >
                      {creating ? `Queueing ${playlist.count} videos...` : `Download all ${playlist.count}`}
                    </button>
                  </div>
                  <ErrorBanner error={createError} />

                  {playlistProgress && (
                    <div style={{ border: "1px solid var(--color-surface-border)", borderRadius: 6, padding: "0.5rem 0.75rem", fontSize: "0.85rem" }} role="status" aria-live="polite">
                      Queued {playlistProgress.total} downloads. {playlistProgress.completed} done
                      {playlistProgress.running > 0 ? `, ${playlistProgress.running} running` : ""}
                      {playlistProgress.failed > 0 ? `, ${playlistProgress.failed} failed` : ""}.
                      <div style={{ color: "var(--color-text-secondary)" }}>
                        They run one at a time. Watch or cancel individual videos on the Queue
                        screen.
                      </div>
                      <div style={{ display: "flex", gap: "0.75rem", marginTop: "0.5rem" }}>
                        <button onClick={() => navigate({ kind: "queue" })}>Open the Queue</button>
                        <button onClick={handleStartOver}>Download something else</button>
                      </div>
                    </div>
                  )}
                </div>
              )}

              {activeJob && (
                <div style={{ border: "1px solid var(--color-surface-border)", borderRadius: 8, padding: "0.75rem", background: "rgba(99, 102, 241, 0.05)", marginTop: "0.75rem" }} ref={jobSectionRef as any} tabIndex={-1}>
                  <div style={{ marginBottom: "0.5rem" }} aria-live="polite">
                    <strong>State:</strong> {activeJob.state}
                  </div>
                  <div style={{ marginBottom: "0.5rem", color: "var(--color-text-secondary)" }} aria-live="polite">{activeJob.progress.statusMessage}</div>
                  {activeJob.progress.percentage !== undefined && (
                    <div
                      style={{ background: "rgba(99, 102, 241, 0.2)", borderRadius: 4, height: 6, overflow: "hidden", marginBottom: "0.5rem" }}
                      role="progressbar"
                      aria-valuenow={Math.round(activeJob.progress.percentage)}
                      aria-valuemin={0}
                      aria-valuemax={100}
                      aria-label="Download progress"
                    >
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
                      <div style={{ display: "flex", gap: "0.75rem", marginTop: "0.5rem", flexWrap: "wrap" }}>
                        <button ref={openFolderButtonRef} onClick={() => void handleOpenFolder()} style={{ padding: "0.4rem 0.8rem", fontSize: "0.85rem", background: "var(--color-accent)", color: "white", border: "none", borderRadius: 4, cursor: "pointer" }}>
                          Open folder
                        </button>
                        <button onClick={handleStartOver} style={{ padding: "0.4rem 0.8rem", fontSize: "0.85rem", background: "var(--color-accent)", color: "white", border: "none", borderRadius: 4, cursor: "pointer" }}>
                          Download another
                        </button>
                      </div>
                      <ErrorBanner error={openFolderError} />
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
  );
}
