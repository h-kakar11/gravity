import { useCallback, useEffect, useMemo, useRef, useState, type CSSProperties } from "react";
import PresetBar from "../components/PresetBar";
import { useJobs } from "../hooks/useJobs";
import { useNavigation } from "../navigation/NavigationContext";
import * as coreClient from "../services/coreClient";
import { asErrorInfo } from "../utils/errors";
import { formatSpeedWithUnit } from "../utils/format";
import type { DownloadMetadata, PlaylistInfo, QualityPreset } from "../types/download";
import { QUALITY_PRESET_LABELS } from "../types/download";
import type { ErrorInfo } from "../types/error";
import type { JobState } from "../types/job";
import type { SpeedUnit } from "../types/settings";
import { analyzePlaylistUrl, joinWindowsPath, withoutPlaylistParam } from "../utils/playlistUrl";

// Functional Phase 2 downloader screen (spec section 34) -- proves URL -> metadata ->
// quality selection -> real download -> progress -> cancellation -> verified completion
// end-to-end through the real architecture (React -> Tauri -> C++ -> DownloadJob ->
// YtDlpProvider -> Python -> yt-dlp). Deliberately basic styling; the final dark-mode home
// screen (docs/roadmap.md "UI") is a later phase.

const QUALITY_OPTIONS: QualityPreset[] = ["BEST", "2160P", "1440P", "1080P", "720P", "480P", "AUDIO_ONLY"];
const ACTIVE_STATES: ReadonlySet<JobState> = new Set(["QUEUED", "STARTING", "RUNNING", "PAUSED"]);

// The backend's answer for "this URL is a playlist, not a single video". Inspecting a bare
// playlist link fails with this, which is the signal to enumerate it instead -- and it works
// on any site, unlike guessing from the URL's shape. See docs/decisions.md, "Playlist URLs".
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

  // --- playlist state (issue #41) ---------------------------------------------------
  // The enumerated playlist, once the user has asked for one. Mutually exclusive with
  // `metadata` in practice: a URL resolves to one video or to a list, never both at once.
  const [playlist, setPlaylist] = useState<PlaylistInfo | null>(null);
  const [playlistLoading, setPlaylistLoading] = useState(false);
  // Set when a pasted link points at one video *inside* a playlist. The user picked "ask me"
  // for this case, so the page offers the choice rather than guessing -- see
  // analyzePlaylistUrl and docs/decisions.md.
  const [comboChoiceUrl, setComboChoiceUrl] = useState<string | null>(null);
  // Destination subfolder for the playlist. Pre-filled with the backend's next unused
  // "playlist #n" so two playlists never land on top of each other, but it is a plain
  // editable field -- the expectation is that the user types the real playlist name.
  const [playlistFolder, setPlaylistFolder] = useState("");
  const [playlistJobIds, setPlaylistJobIds] = useState<string[]>([]);

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

  // Enumerates a playlist and pre-fills its destination folder. Shared by the bare-playlist
  // path (reached when `inspect` reports the URL is a list) and by the user answering
  // "download the whole playlist" to a combo link.
  const loadPlaylist = useCallback(
    async (target: string) => {
      setPlaylistLoading(true);
      setInspectError(null);
      setMetadata(null);
      setPlaylist(null);
      try {
        const { playlist: result } = await coreClient.inspectPlaylistUrl(target);
        setPlaylist(result);
        // A suggestion, not a reservation -- see HandleSuggestPlaylistFolder in main.cpp.
        // Failing to get one is not worth blocking the download over; the user can type a
        // name themselves, and the field simply starts on the playlist's own title.
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

  // `urlOverride` lets a paste handler kick off inspection with the just-pasted text
  // immediately, without waiting for a `setUrl` re-render to land in the `url` state this
  // callback otherwise closes over (issue #62).
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
        // Inspect resolved one video, but the link also carries a playlist reference -- the
        // "shared from a playlist" case. Offer the choice rather than assuming either way.
        if (analyzePlaylistUrl(target).hasPlaylist) {
          setComboChoiceUrl(target);
        }
      } catch (err) {
        const info = asErrorInfo(err);
        // Not an error the user needs to see: it means "this is a playlist", which is now a
        // supported thing to be. Enumerate it instead.
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

  // Auto-detect a pasted link and inspect it immediately instead of requiring a separate
  // click on Inspect (issue #62). Only fires for something that already looks like an
  // http(s) URL -- ValidateDownloadUrl/CanHandle on the core side is the real gate, this
  // is just "don't try to auto-inspect someone pasting arbitrary text."
  //
  // preventDefault() is load-bearing, not tidiness (issue #81: "Ctrl+V pastes the URL
  // twice"). `paste` is a DISCRETE event in React 18, so setUrl() below is flushed
  // synchronously when this handler returns -- which writes the pasted text into the
  // controlled input's DOM value and leaves the caret at the end -- and only THEN does the
  // browser run the paste's default action, inserting the same text a second time at that
  // caret. The field ends up holding the URL twice over. Taking over the insertion
  // entirely is the fix; the state we set here is already exactly what the default action
  // would have produced.
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

  // Fans a playlist out into one DownloadJob per entry, chained so they run strictly one at
  // a time: each job declares the previous one as a `runAfter`.
  //
  // `runAfter`, not `dependsOn`, and the difference matters: `dependsOn` requires the
  // predecessor to COMPLETE, and cancels its dependents when it doesn't -- so a single
  // unavailable video (routine in a long playlist) would cancel every entry after it.
  // `runAfter` only waits for the predecessor to reach a terminal state, so one failure
  // costs exactly one video. See SchedulerCore::Submission.
  //
  // Jobs are created sequentially rather than in parallel because entry N+1 needs entry N's
  // id, which only exists once its createJob call has returned.
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
      // Partial failure is real: entries before the failure are already queued and running.
      // Record what was created so the UI reports the true count rather than implying
      // nothing happened.
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
    setPlaylist(null);
    setPlaylistJobIds([]);
    setPlaylistFolder("");
    setComboChoiceUrl(null);
  }, []);

  // Live rollup of the fanned-out jobs, read straight from the shared job list rather than
  // tracked separately -- the queue is the source of truth for how a playlist is going.
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

  // Distinct from handleStartOver: retrying a FAILED job should not drop the user back to
  // a blank paste screen, or make them re-inspect a URL that was already successfully
  // resolved -- only the download itself failed. See issue #56.
  const handleRetry = useCallback(() => {
    setActiveJobId(null);
    setCreateError(null);
  }, []);

  // Deliberate focus management (issue #32): the Download button is replaced by Cancel
  // (or the whole controls row disappears once a job exists), so without this, focus
  // silently falls back to <body> at exactly the moment something important happens --
  // the job starting, failing, or completing.
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
            onPaste={handlePasteUrl}
            onKeyDown={(e) => {
              if (e.key === "Enter" && !inspecting && url.trim() && !canCancel) void handleInspect();
            }}
            disabled={canCancel}
          />
          <button onClick={() => void handleInspect()} disabled={inspecting || !url.trim() || canCancel}>
            {inspecting ? "Inspecting..." : "Inspect"}
          </button>
        </div>
        {/* A bare input with no context otherwise -- issue #33 item 7. A link to one video
            that also happens to be in a playlist offers the choice below rather than
            guessing; a link to the playlist itself is enumerated (issue #41). */}
        {!metadata && !playlist && !inspecting && !playlistLoading && !inspectError ? (
          <p style={styles.muted}>
            Works with YouTube and most other sites yt-dlp supports. Paste a video or playlist
            link above and click Inspect to preview it before downloading.
          </p>
        ) : null}
        {playlistLoading ? <p style={styles.muted}>Reading playlist...</p> : null}
        <ErrorBanner error={inspectError} />

        {/* The "shared from a playlist" fork. Deliberately shown after the single video
            resolves, so choosing "just this video" needs no further waiting -- the metadata
            is already on screen. */}
        {comboChoiceUrl ? (
          <div style={styles.choiceBanner} role="group" aria-label="This link is part of a playlist">
            <div>This link is part of a playlist.</div>
            <div style={styles.row}>
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
        ) : null}

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
              {/* minWidth: 0 overrides the flex item's default min-width: auto, which
                  would otherwise let a long title push this column wider than the flex
                  container instead of actually wrapping (issue #33 item 2). */}
              <div style={{ minWidth: 0 }}>
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

      {playlist ? (
        <section style={styles.section}>
          <h2 style={{ ...styles.h2, ...styles.overflowSafe }}>{playlist.title}</h2>
          <p style={styles.muted}>
            {playlist.count} video{playlist.count === 1 ? "" : "s"}
            {playlist.uploader ? ` · ${playlist.uploader}` : ""} · downloaded one at a time, in
            order.
          </p>
          {playlist.truncated ? (
            <div style={styles.warnBanner} role="status">
              This playlist is longer than the {playlist.count}-video limit. Only the first{" "}
              {playlist.count} will be downloaded.
            </div>
          ) : null}
          {playlist.unavailableCount > 0 ? (
            <div style={styles.warnBanner} role="status">
              {playlist.unavailableCount} video{playlist.unavailableCount === 1 ? " is" : "s are"}{" "}
              unavailable (deleted or private) and {playlist.unavailableCount === 1 ? "was" : "were"}{" "}
              skipped.
            </div>
          ) : null}

          <details style={styles.formatsDisclosure}>
            <summary>Show the {playlist.count} videos</summary>
            <ol style={styles.playlistList}>
              {playlist.entries.map((entry) => (
                <li key={`${entry.index}-${entry.url}`} style={styles.overflowSafe}>
                  {entry.title}
                  {entry.durationSeconds !== undefined
                    ? ` · ${formatDuration(entry.durationSeconds)}`
                    : ""}
                </li>
              ))}
            </ol>
          </details>

          <div style={styles.row}>
            <label style={styles.label}>
              Quality for all videos:{" "}
              <select
                value={quality}
                onChange={(e) => setQuality(e.target.value as QualityPreset)}
                disabled={creating}
              >
                {QUALITY_OPTIONS.map((preset) => (
                  <option key={preset} value={preset}>
                    {QUALITY_PRESET_LABELS[preset]}
                  </option>
                ))}
              </select>
            </label>
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
                disabled={creating}
              />
            </label>
          </div>
          <div style={styles.row}>
            <label style={styles.label}>
              Playlist folder name:{" "}
              <input
                style={styles.input}
                type="text"
                placeholder="playlist #1"
                value={playlistFolder}
                onChange={(e) => setPlaylistFolder(e.target.value)}
                disabled={creating}
              />
            </label>
          </div>
          {/* Showing the resolved destination because two editable fields combine into it,
              and "where did my 47 files go" is otherwise only answerable after the fact. */}
          {outputDirectory.trim() && playlistFolder.trim() ? (
            <p style={{ ...styles.muted, ...styles.overflowSafe }}>
              Files go to {joinWindowsPath(outputDirectory.trim(), playlistFolder.trim())}, named{" "}
              <code>01 - Title</code>, <code>02 - Title</code>, ...
            </p>
          ) : null}

          <div style={styles.row}>
            <button
              onClick={() => void handleDownloadPlaylist()}
              disabled={
                creating ||
                playlist.entries.length === 0 ||
                !outputDirectory.trim() ||
                !playlistFolder.trim() ||
                playlistJobIds.length > 0
              }
            >
              {creating
                ? `Queueing ${playlist.count} videos...`
                : `Download all ${playlist.count}`}
            </button>
          </div>
          <ErrorBanner error={createError} />

          {playlistProgress ? (
            <div style={styles.okBanner} role="status" aria-live="polite">
              Queued {playlistProgress.total} downloads. {playlistProgress.completed} done
              {playlistProgress.running > 0 ? `, ${playlistProgress.running} running` : ""}
              {playlistProgress.failed > 0 ? `, ${playlistProgress.failed} failed` : ""}.
              <div style={styles.muted}>
                They run one at a time. Watch or cancel individual videos on the Queue screen.
              </div>
              <div style={styles.row}>
                <button onClick={handleStartOver}>Download something else</button>
              </div>
            </div>
          ) : null}
        </section>
      ) : null}

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
        // tabIndex=-1: focusable via script (see the activeJobId effect above) without
        // joining the normal Tab order.
        <section style={styles.section} ref={jobSectionRef} tabIndex={-1}>
          {/* The raw job id ("Job job-3f2a8c1d-...") read as developer output, not a
              product heading -- issue #33 item 5. metadata is still in scope here (kept
              across a retry, only cleared by Start Over), so the video's own title is
              almost always available; the id is a last-resort fallback, not the norm. */}
          <h2 style={{ ...styles.h2, ...styles.overflowSafe }}>{metadata?.title ?? `Job ${activeJob.id}`}</h2>
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
                  <div style={{ ...styles.muted, ...styles.overflowSafe }}>{activeJob.result.outputPath}</div>
                ) : null}
                <div style={styles.row}>
                  <button ref={openFolderButtonRef} onClick={() => void handleOpenFolder()}>
                    Open folder
                  </button>
                  <button onClick={handleStartOver}>Download another</button>
                </div>
                <ErrorBanner error={openFolderError} />
              </div>
            ) : null}

            {activeJob.state === "FAILED" && activeJob.error ? (
              <div ref={failureRef} tabIndex={-1}>
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
  // Long video titles (the sanitizer allows up to 200 codepoints) and deep Windows output
  // paths would otherwise force horizontal scrolling or overflow the fixed-width page --
  // issue #33 item 2.
  overflowSafe: { overflowWrap: "break-word", wordBreak: "break-word" },
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
  title: { fontWeight: 600, color: "#1a1a1a", overflowWrap: "break-word", wordBreak: "break-word" },
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
  choiceBanner: {
    border: "1px solid #b6d4fe",
    background: "#cfe2ff",
    color: "#084298",
    borderRadius: 6,
    padding: "0.5rem 0.75rem",
    fontSize: "0.9rem",
    display: "flex",
    flexDirection: "column",
    gap: "0.4rem",
  },
  warnBanner: {
    border: "1px solid #ffe69c",
    background: "#fff3cd",
    color: "#664d03",
    borderRadius: 6,
    padding: "0.5rem 0.75rem",
    fontSize: "0.9rem",
  },
  playlistList: {
    margin: "0.4rem 0 0",
    paddingLeft: "1.6rem",
    fontSize: "0.8rem",
    color: "#444",
    maxHeight: 260,
    overflowY: "auto",
  },
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
