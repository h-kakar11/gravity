import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { open as openFilePicker } from "@tauri-apps/plugin-dialog";
import GlassCard from "../components/GlassCard";
import PresetBar from "../components/PresetBar";
import { useJobs } from "../hooks/useJobs";
import { useNavigation } from "../navigation/NavigationContext";
import * as coreClient from "../services/coreClient";
import type { FileCategory, FileInfo } from "../types/fileInfo";
import type { ErrorInfo } from "../types/error";
import type { JobSnapshot } from "../types/job";
import {
  CONVERSION_QUALITY_TIERS,
  LOSSY_QUALITY_TIERS,
  QUALITY_LABELS,
  type HardwareAcceleration,
  type MediaProcessingOptions,
  type MediaQuality,
  type VideoCodec,
} from "../types/conversion";
import { asErrorInfo } from "../utils/errors";
import { formatBytes, formatBytesPerSecond, formatEta } from "../utils/format";
import styles from "./ConvertPage.module.css";

// Real Convert/Compress screen (Phase 2.6's engine finally gets a page): drop or prefill
// local files -> pick an output format/quality -> real ffmpeg jobs -> live progress ->
// verified completion. Convert and Compress are the same form submitting to different
// createJob types (docs/decisions.md: Compress is Convert with different default option
// values, not a different code path) -- so this is one component with a mode toggle, not two.
//
// The screen takes a BATCH, not a file. It used to hold a single `inputPath`, open its
// picker with `multiple: false`, keep only `paths[0]` from a drop, and disable its own
// submit button while a job was in flight -- so converting a folder of videos meant
// repeating the whole flow by hand, once per file, waiting each time. Everything below is
// per-item where it has to be (one job, one format, one progress row) and shared where it
// can be (quality, codec, output folder), because a batch whose settings had to be chosen
// file by file would be no better than the loop it replaces.

type Mode = "convert" | "compress";

// Only categories with somewhere to convert TO can be batch items; everything else is
// reported as skipped rather than queued into a job that would fail.
const FORMAT_OPTIONS_BY_CATEGORY: Record<FileCategory, string[]> = {
  VIDEO: ["mp4", "webm", "mov", "mkv", "gif"],
  AUDIO: ["mp3", "wav", "flac", "aac", "m4a", "ogg"],
  IMAGE: ["png", "jpg", "webp", "gif", "bmp"],
  DOCUMENT: [],
  TEXT: [],
  ARCHIVE: [],
  UNKNOWN: [],
};

// Ordering for anything that shows one row per category, so a mixed batch always reads the
// same way round rather than in whichever order the files happened to arrive.
const CATEGORY_ORDER: FileCategory[] = ["VIDEO", "AUDIO", "IMAGE"];

const CATEGORY_LABELS: Partial<Record<FileCategory, string>> = {
  VIDEO: "Video",
  AUDIO: "Audio",
  IMAGE: "Image",
};

const TERMINAL_STATES = ["COMPLETED", "FAILED", "CANCELLED"];

function isConvertible(category: FileCategory): boolean {
  return FORMAT_OPTIONS_BY_CATEGORY[category].length > 0;
}

// One file the user has lined up. `jobId` is filled in on submit; `error` is either why the
// file could not be added at all or why its job could not be created.
interface BatchItem {
  path: string;
  info: FileInfo | null;
  jobId: string | null;
  error: ErrorInfo | null;
}

// Runs `fn` over `items` with at most `limit` in flight. Inspecting a file is an ffprobe
// run behind a four-thread executor in the core (docs/ipc-contract.md), so firing a
// Promise.all over a 200-file drop would spend most of its round trips collecting
// E_CORE_BUSY; a plain sequential loop would instead leave three of those threads idle.
async function mapWithConcurrency<T, R>(
  items: readonly T[],
  limit: number,
  fn: (item: T, index: number) => Promise<R>,
): Promise<R[]> {
  const results = new Array<R>(items.length);
  let next = 0;
  const workers = Array.from({ length: Math.min(limit, items.length) }, async () => {
    for (let i = next++; i < items.length; i = next++) {
      results[i] = await fn(items[i], i);
    }
  });
  await Promise.all(workers);
  return results;
}

function ErrorBanner({ error }: { error: ErrorInfo | null }) {
  if (!error) return null;
  return (
    <div className={styles.errorBanner}>
      <strong>{error.category}</strong> ({error.code}): {error.message}
    </div>
  );
}

export default function ConvertPage() {
  const { screen, navigate } = useNavigation();
  const prefill = screen.kind === "convert" ? screen : null;

  const [mode, setMode] = useState<Mode>(prefill?.mode ?? "convert");
  const [items, setItems] = useState<BatchItem[]>([]);
  const [adding, setAdding] = useState(false);
  // What the last add left out: files that are not convertible at all, and whether a
  // folder held more than one listing returns. Shown once rather than as a row per file.
  const [skippedCount, setSkippedCount] = useState(0);
  const [truncated, setTruncated] = useState(false);
  const [addError, setAddError] = useState<ErrorInfo | null>(null);

  // One chosen output format per category present in the batch. A single-category batch
  // (the common case) renders exactly the one dropdown the old single-file form had; a
  // folder holding video and stills gets one row each, because "convert everything to mp4"
  // is not a thing to do to a PNG.
  const [formatByCategory, setFormatByCategory] = useState<Partial<Record<FileCategory, string>>>({});
  const [quality, setQuality] = useState<MediaQuality>("medium");
  const [defaultCompressionQuality, setDefaultCompressionQuality] = useState<MediaQuality>("medium");
  // Whether the user has picked a quality themselves -- see the Settings seed below.
  const qualityTouched = useRef(false);
  const [videoCodec, setVideoCodec] = useState<VideoCodec>("auto");
  const [hardwareAcceleration, setHardwareAcceleration] = useState<HardwareAcceleration>("auto");
  const [audioBitrateKbps, setAudioBitrateKbps] = useState<string>("");
  const [outputDirectory, setOutputDirectory] = useState("");

  const [creating, setCreating] = useState(false);
  const [createError, setCreateError] = useState<ErrorInfo | null>(null);
  const [submitted, setSubmitted] = useState(false);
  const [openFolderError, setOpenFolderError] = useState<ErrorInfo | null>(null);

  const { jobs, cancelJob } = useJobs();

  // Seed the output directory from Settings once, so the user isn't stuck typing a path by
  // hand for the common case -- they can still override it per batch. Same for the default
  // compression quality: Settings has advertised one since issue #59, but nothing ever
  // read it here, so picking "Ultra low" there changed nothing about an actual job.
  useEffect(() => {
    coreClient
      .getSettings()
      .then(({ settings }) => {
        setOutputDirectory(settings.general.defaultOutputDirectory);
        const configured = settings.processing.defaultCompressionQuality as MediaQuality;
        if (!LOSSY_QUALITY_TIERS.includes(configured)) return;
        setDefaultCompressionQuality(configured);
        // Settings arrives asynchronously, so this can land after the user has already
        // picked a tier -- adopting it then would silently discard their choice.
        if (!qualityTouched.current) setQuality(configured);
      })
      .catch(() => {
        // Non-fatal -- the fields just start at their existing defaults.
      });
    // Deliberately mount-only: this seeds initial values, it does not track Settings.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Compression never offers "lossless" (see types/conversion.ts), so switching into it
  // has to pull an out-of-range selection back rather than submit a tier the mode doesn't
  // support.
  const qualityTiers = mode === "compress" ? LOSSY_QUALITY_TIERS : CONVERSION_QUALITY_TIERS;
  useEffect(() => {
    if (mode !== "compress") return;
    setQuality((current) =>
      LOSSY_QUALITY_TIERS.includes(current) ? current : defaultCompressionQuality,
    );
  }, [mode, defaultCompressionQuality]);

  const chooseQuality = useCallback((next: MediaQuality) => {
    qualityTouched.current = true;
    setQuality(next);
  }, []);

  // --- building the batch ------------------------------------------------------------

  // Adds every path the user handed over, expanding any that turn out to be folders.
  //
  // A dropped path carries no hint of whether it is a file or a directory, and the OS
  // drag-drop payload for "the user dragged a folder in" is just that folder's path. So
  // each path is inspected first, and anything that comes back as a non-convertible
  // category is offered to listFolderFiles before being written off -- a folder inspects
  // to UNKNOWN (it has no extension), which is exactly the case worth a second look. That
  // second round trip only ever happens for paths that were not usable as files anyway.
  const addPaths = useCallback(async (paths: string[]) => {
    if (paths.length === 0) return;
    setAdding(true);
    setAddError(null);

    const added: BatchItem[] = [];
    let skipped = 0;
    let wasTruncated = false;

    try {
      const resolved = await mapWithConcurrency(paths, 4, async (path) => {
        try {
          const { fileInfo } = await coreClient.inspectFile(path);
          if (isConvertible(fileInfo.category)) {
            return { kind: "file" as const, info: fileInfo };
          }
        } catch (err) {
          // Fall through: a path that cannot be inspected as a file may still be a folder.
          const info = asErrorInfo(err);
          const folder = await tryListFolder(path);
          if (folder) return folder;
          return { kind: "error" as const, path, error: info };
        }
        const folder = await tryListFolder(path);
        if (folder) return folder;
        return { kind: "skipped" as const };
      });

      for (const entry of resolved) {
        if (entry.kind === "file") {
          added.push({ path: entry.info.path, info: entry.info, jobId: null, error: null });
        } else if (entry.kind === "folder") {
          for (const info of entry.files) {
            added.push({ path: info.path, info, jobId: null, error: null });
          }
          skipped += entry.skipped;
          wasTruncated = wasTruncated || entry.truncated;
        } else if (entry.kind === "error") {
          added.push({ path: entry.path, info: null, jobId: null, error: entry.error });
        } else {
          skipped += 1;
        }
      }
    } catch (err) {
      setAddError(asErrorInfo(err));
    } finally {
      setAdding(false);
    }

    setSkippedCount(skipped);
    setTruncated(wasTruncated);
    setItems((prev) => {
      // Deduplicated by path: dropping the same file twice, or a file and then the folder
      // holding it, should not queue it twice.
      const seen = new Set(prev.map((item) => item.path));
      const fresh: BatchItem[] = [];
      for (const item of added) {
        if (seen.has(item.path)) continue;
        seen.add(item.path);
        fresh.push(item);
      }
      return [...prev, ...fresh];
    });
  }, []);

  const removeItem = useCallback((path: string) => {
    setItems((prev) => prev.filter((item) => item.path !== path));
  }, []);

  const handleStartOver = useCallback(() => {
    setItems([]);
    setSubmitted(false);
    setCreateError(null);
    setAddError(null);
    setSkippedCount(0);
    setTruncated(false);
  }, []);

  // File-picker fallback alongside drag-and-drop (issue #57: "should have a selection of
  // what file to convert rather than relying on solely drag and drop"), now multi-select.
  const handleBrowseFiles = useCallback(async () => {
    const selected = await openFilePicker({
      multiple: true,
      title: "Choose files to convert or compress",
    });
    const paths = Array.isArray(selected) ? selected : typeof selected === "string" ? [selected] : [];
    await addPaths(paths);
  }, [addPaths]);

  // Straight to listFolderFiles rather than through addPaths: the directory picker already
  // guarantees this is a folder, so the "inspect it as a file first, then reconsider" dance
  // addPaths does for an ambiguous dropped path would just be a wasted round trip.
  const handleBrowseFolder = useCallback(async () => {
    const selected = await openFilePicker({
      directory: true,
      multiple: false,
      title: "Choose a folder to convert",
    });
    if (typeof selected !== "string") return;

    setAdding(true);
    setAddError(null);
    try {
      const { files, skipped, truncated: wasTruncated } = await coreClient.listFolderFiles(selected);
      setSkippedCount(skipped);
      setTruncated(wasTruncated);
      setItems((prev) => {
        const seen = new Set(prev.map((item) => item.path));
        const fresh: BatchItem[] = [];
        for (const info of files) {
          if (seen.has(info.path)) continue;
          seen.add(info.path);
          fresh.push({ path: info.path, info, jobId: null, error: null });
        }
        return [...prev, ...fresh];
      });
    } catch (err) {
      setAddError(asErrorInfo(err));
    } finally {
      setAdding(false);
    }
  }, []);

  // Re-runs when the screen is navigated to with a new set of paths (the Windows context
  // menu opening a second file into an already-running instance, App.tsx's
  // cli-file-opened). addPaths deduplicates, so a repeat of the same list is a no-op.
  const prefillKey = prefill?.prefillFilePaths?.join("\u0000") ?? "";
  useEffect(() => {
    if (prefillKey.length === 0) return;
    void addPaths(prefillKey.split("\u0000"));
  }, [prefillKey, addPaths]);

  // --- what the batch contains -------------------------------------------------------

  const categoriesPresent = useMemo(() => {
    const present = new Set(items.map((item) => item.info?.category).filter(Boolean) as FileCategory[]);
    return CATEGORY_ORDER.filter((category) => present.has(category));
  }, [items]);

  // Every category present needs a format before anything can be submitted; default each
  // to the first option the moment that category appears.
  useEffect(() => {
    setFormatByCategory((prev) => {
      let changed = false;
      const next = { ...prev };
      for (const category of categoriesPresent) {
        if (!next[category]) {
          next[category] = FORMAT_OPTIONS_BY_CATEGORY[category][0];
          changed = true;
        }
      }
      return changed ? next : prev;
    });
  }, [categoriesPresent]);

  const readyItems = useMemo(() => items.filter((item) => item.info !== null), [items]);
  const hasVideo = categoriesPresent.includes("VIDEO");
  const hasAudioTrack = hasVideo || categoriesPresent.includes("AUDIO");
  // What PresetBar saves and applies. A preset is one set of options, so a mixed batch has
  // to nominate one category for it; video first, matching CATEGORY_ORDER.
  const primaryCategory = categoriesPresent[0] ?? null;

  const totalBytes = useMemo(
    () => readyItems.reduce((sum, item) => sum + (item.info?.sizeBytes ?? 0), 0),
    [readyItems],
  );

  const canSubmit =
    readyItems.length > 0 &&
    outputDirectory.trim().length > 0 &&
    !creating &&
    !adding &&
    categoriesPresent.every((category) => Boolean(formatByCategory[category]));

  // Plain function (not memoized): both handleSubmit and PresetBar's "save as preset"
  // button need the options object built from whatever the form currently holds, and
  // PresetBar re-renders alongside this component on every state change anyway.
  //
  // Per category, because the options that make sense are not the same for every file in a
  // mixed batch: a codec and hardware encoder only mean something for video, an audio
  // bitrate only for something with an audio track.
  const buildOptions = (category: FileCategory | null): MediaProcessingOptions => ({
    outputFormat: (category && formatByCategory[category]) || "",
    quality,
    ...(category === "VIDEO" ? { videoCodec, hardwareAcceleration } : {}),
    ...(("VIDEO" === category || "AUDIO" === category) && audioBitrateKbps.trim()
      ? { audioBitrateKbps: Number(audioBitrateKbps) }
      : {}),
  });

  const applyPresetOptions = useCallback(
    (options: Record<string, unknown>) => {
      const opts = options as Partial<MediaProcessingOptions>;
      if (typeof opts.outputFormat === "string") {
        // Applied to whichever category actually offers that format, so loading a "MP3
        // 320" preset over a mixed batch changes the audio row and leaves the video row
        // alone rather than writing "mp3" into a field that would fail the job.
        const target =
          CATEGORY_ORDER.find(
            (category) =>
              categoriesPresent.includes(category) &&
              FORMAT_OPTIONS_BY_CATEGORY[category].includes(opts.outputFormat as string),
          ) ?? primaryCategory;
        if (target) {
          const format = opts.outputFormat;
          setFormatByCategory((prev) => ({ ...prev, [target]: format }));
        }
      }
      if (opts.quality) {
        qualityTouched.current = true;
        setQuality(opts.quality);
      }
      if (opts.videoCodec) setVideoCodec(opts.videoCodec);
      if (opts.hardwareAcceleration) setHardwareAcceleration(opts.hardwareAcceleration);
      if (typeof opts.audioBitrateKbps === "number") setAudioBitrateKbps(String(opts.audioBitrateKbps));
    },
    [categoriesPresent, primaryCategory],
  );

  // --- submitting the batch ----------------------------------------------------------

  // One createJob per file, in list order. Sequential on purpose: the core's queue is FIFO
  // within a priority band, so submitting in order is what makes the jobs run in the order
  // the user is looking at. They then run N-at-a-time on the worker pool
  // (processing.concurrentJobs), which is where the actual parallelism lives -- this loop
  // is submission, not execution, and each call is a memory-speed round trip.
  //
  // A file the core rejects (it vanished between listing and submitting, the output drive
  // filled up) records its error against that row and the loop carries on: one bad file
  // out of eighty must not cost the other seventy-nine.
  const handleSubmit = useCallback(async () => {
    if (readyItems.length === 0) return;
    setCreating(true);
    setCreateError(null);

    const results = new Map<string, { jobId: string | null; error: ErrorInfo | null }>();
    for (const item of readyItems) {
      const category = item.info?.category ?? null;
      const params = {
        inputPath: item.path,
        outputDirectory: outputDirectory.trim(),
        options: buildOptions(category),
      };
      try {
        const { jobId } =
          mode === "convert"
            ? await coreClient.createConversionJob(params)
            : await coreClient.createCompressionJob(params);
        results.set(item.path, { jobId, error: null });
      } catch (err) {
        results.set(item.path, { jobId: null, error: asErrorInfo(err) });
      }
    }

    setItems((prev) =>
      prev.map((item) => {
        const result = results.get(item.path);
        return result ? { ...item, jobId: result.jobId, error: result.error } : item;
      }),
    );
    if (results.size > 0 && [...results.values()].every((r) => r.jobId === null)) {
      // Every single one failed, which is a batch-level problem (a bad output folder, a
      // full disk) rather than N unrelated per-file ones -- say it once, at the top.
      setCreateError([...results.values()][0].error);
    }
    setSubmitted(true);
    setCreating(false);
  }, [readyItems, outputDirectory, mode, quality, videoCodec, hardwareAcceleration, audioBitrateKbps, formatByCategory]);

  // --- watching the batch ------------------------------------------------------------

  const jobsById = useMemo(() => {
    const map = new Map<string, JobSnapshot>();
    for (const job of jobs) map.set(job.id, job);
    return map;
  }, [jobs]);

  const submittedItems = useMemo(() => items.filter((item) => item.jobId !== null), [items]);
  const batchJobs = useMemo(
    () => submittedItems.map((item) => jobsById.get(item.jobId as string)).filter(Boolean) as JobSnapshot[],
    [submittedItems, jobsById],
  );

  const completedCount = batchJobs.filter((job) => job.state === "COMPLETED").length;
  const failedCount =
    batchJobs.filter((job) => job.state === "FAILED").length +
    items.filter((item) => item.jobId === null && item.error !== null && submitted).length;
  const cancelledCount = batchJobs.filter((job) => job.state === "CANCELLED").length;
  const activeJobs = batchJobs.filter((job) => !TERMINAL_STATES.includes(job.state));
  const batchInFlight = activeJobs.length > 0;

  const handleCancelAll = useCallback(async () => {
    // Snapshotted first: cancelling mutates the list this is iterating over.
    const ids = activeJobs.map((job) => job.id);
    await Promise.all(ids.map((id) => cancelJob(id).catch(() => undefined)));
  }, [activeJobs, cancelJob]);

  const firstOutputPath = useMemo(() => {
    for (const job of batchJobs) {
      const outputPath = job.result?.outputPath;
      if (job.state === "COMPLETED" && typeof outputPath === "string") return outputPath;
    }
    return null;
  }, [batchJobs]);

  const handleOpenFolder = useCallback(async () => {
    if (!firstOutputPath) return;
    setOpenFolderError(null);
    try {
      await coreClient.openContainingFolder(firstOutputPath);
    } catch (err) {
      setOpenFolderError(asErrorInfo(err));
    }
  }, [firstOutputPath]);

  // --- render -------------------------------------------------------------------------

  const skippedNotice = [
    skippedCount > 0
      ? `${skippedCount} file${skippedCount === 1 ? "" : "s"} skipped (nothing to convert them to).`
      : null,
    truncated ? "That folder holds more files than one batch can take; the first 500 were added." : null,
  ]
    .filter(Boolean)
    .join(" ");

  return (
    <div className={styles.wrap}>
      <div className={styles.header}>
        <h1 className={styles.title}>Convert &amp; Compress</h1>
        <div className={styles.modeToggle}>
          <button
            className={`${styles.modeButton} ${mode === "convert" ? styles.modeButtonActive : ""}`}
            onClick={() => setMode("convert")}
            disabled={submitted}
          >
            Convert
          </button>
          <button
            className={`${styles.modeButton} ${mode === "compress" ? styles.modeButtonActive : ""}`}
            onClick={() => setMode("compress")}
            disabled={submitted}
          >
            Compress
          </button>
        </div>
      </div>

      {items.length === 0 ? (
        <GlassCard
          className={styles.dropzone}
          ariaLabel="Drop files or a folder to convert or compress"
          onFilesDropped={(paths) => void addPaths(paths)}
        >
          <div className={styles.dropzoneInner}>
            <div className={styles.dropIcon}>&#8646;</div>
            <p>Drop files &mdash; or a whole folder &mdash; here to get started.</p>
            <div className={styles.dropzoneActions}>
              <button
                className={styles.linkButton}
                onClick={(e) => {
                  e.stopPropagation();
                  void handleBrowseFiles();
                }}
              >
                Choose files...
              </button>
              <button
                className={styles.linkButton}
                onClick={(e) => {
                  e.stopPropagation();
                  void handleBrowseFolder();
                }}
              >
                Choose a folder...
              </button>
            </div>
            {adding && <div className={styles.fileMeta}>Reading files...</div>}
            <ErrorBanner error={addError} />
          </div>
        </GlassCard>
      ) : (
        <>
          <GlassCard className={styles.section}>
            <div className={styles.fileRow}>
              <div>
                <div className={styles.fileName}>
                  {readyItems.length} file{readyItems.length === 1 ? "" : "s"} selected
                </div>
                <div className={styles.fileMeta}>
                  {categoriesPresent.map((category) => CATEGORY_LABELS[category]).join(" \u00b7 ")}
                  {totalBytes > 0 ? ` \u00b7 ${formatBytes(totalBytes)}` : ""}
                  {adding ? " \u00b7 Reading files..." : ""}
                </div>
              </div>
              {!submitted && (
                <div className={styles.headerActions}>
                  <button className={styles.linkButton} onClick={() => void handleBrowseFiles()}>
                    Add files
                  </button>
                  <button className={styles.linkButton} onClick={() => void handleBrowseFolder()}>
                    Add folder
                  </button>
                  <button className={styles.linkButton} onClick={handleStartOver}>
                    Clear
                  </button>
                </div>
              )}
            </div>
            {skippedNotice && <div className={styles.notice}>{skippedNotice}</div>}
            <ErrorBanner error={addError} />

            <ul className={styles.itemList}>
              {items.map((item) => {
                const job = item.jobId ? jobsById.get(item.jobId) : undefined;
                return (
                  <li key={item.path} className={styles.item}>
                    <div className={styles.itemMain}>
                      <div className={styles.itemName} title={item.path}>
                        {item.info?.filename ?? item.path}
                      </div>
                      <div className={styles.itemMeta}>
                        {item.info
                          ? `${item.info.category} \u00b7 ${formatBytes(item.info.sizeBytes)}`
                          : (item.error?.message ?? "Could not read this file")}
                        {job?.progress.statusMessage ? ` \u00b7 ${job.progress.statusMessage}` : ""}
                        {!job && item.error && item.info ? ` \u00b7 ${item.error.message}` : ""}
                      </div>
                      {job && job.progress.percentage !== undefined && !TERMINAL_STATES.includes(job.state) && (
                        <div className={styles.progressTrack}>
                          <div className={styles.progressFill} style={{ width: `${job.progress.percentage}%` }} />
                        </div>
                      )}
                      {job && !TERMINAL_STATES.includes(job.state) && (
                        <div className={styles.metaRow}>
                          {formatBytesPerSecond(job.progress.speedBytesPerSecond) && (
                            <span>{formatBytesPerSecond(job.progress.speedBytesPerSecond)}</span>
                          )}
                          {formatEta(job.progress.etaSeconds) && <span>{formatEta(job.progress.etaSeconds)}</span>}
                        </div>
                      )}
                    </div>
                    {job ? (
                      <span className={styles.stateBadge}>{job.state}</span>
                    ) : item.error && submitted ? (
                      <span className={styles.stateBadge}>FAILED</span>
                    ) : !submitted ? (
                      <button
                        className={styles.removeButton}
                        onClick={() => removeItem(item.path)}
                        aria-label={`Remove ${item.info?.filename ?? item.path}`}
                      >
                        &times;
                      </button>
                    ) : null}
                  </li>
                );
              })}
            </ul>
          </GlassCard>

          {!submitted && readyItems.length > 0 && (
            <GlassCard className={styles.section}>
              <PresetBar
                kind={mode === "convert" ? "CONVERSION" : "COMPRESSION"}
                currentOptions={() => buildOptions(primaryCategory) as unknown as Record<string, unknown>}
                onApply={applyPresetOptions}
              />
              {categoriesPresent.map((category) => (
                <div className={styles.field} key={category}>
                  <label className={styles.fieldLabel} htmlFor={`format-${category}`}>
                    {categoriesPresent.length > 1
                      ? `Output format (${CATEGORY_LABELS[category]})`
                      : "Output format"}
                  </label>
                  <select
                    id={`format-${category}`}
                    className={styles.selectInput}
                    value={formatByCategory[category] ?? ""}
                    onChange={(e) =>
                      setFormatByCategory((prev) => ({ ...prev, [category]: e.target.value }))
                    }
                  >
                    {FORMAT_OPTIONS_BY_CATEGORY[category].map((fmt) => (
                      <option key={fmt} value={fmt}>
                        .{fmt}
                      </option>
                    ))}
                  </select>
                </div>
              ))}

              <div className={styles.field}>
                <label className={styles.fieldLabel} htmlFor="quality">
                  Quality
                </label>
                <select
                  id="quality"
                  className={styles.selectInput}
                  value={quality}
                  onChange={(e) => chooseQuality(e.target.value as MediaQuality)}
                >
                  {qualityTiers.map((tier) => (
                    <option key={tier} value={tier}>
                      {QUALITY_LABELS[tier]}
                    </option>
                  ))}
                </select>
              </div>

              {hasVideo && (
                <>
                  <div className={styles.field}>
                    <label className={styles.fieldLabel} htmlFor="videoCodec">
                      Video codec
                    </label>
                    <select
                      id="videoCodec"
                      className={styles.selectInput}
                      value={videoCodec}
                      onChange={(e) => setVideoCodec(e.target.value as VideoCodec)}
                    >
                      <option value="auto">Auto</option>
                      <option value="h264">H.264</option>
                      <option value="h265">H.265</option>
                      <option value="vp9">VP9</option>
                      <option value="av1">AV1</option>
                    </select>
                  </div>
                  <div className={styles.field}>
                    <label className={styles.fieldLabel} htmlFor="hardwareAcceleration">
                      Hardware acceleration
                    </label>
                    <select
                      id="hardwareAcceleration"
                      className={styles.selectInput}
                      value={hardwareAcceleration}
                      onChange={(e) => setHardwareAcceleration(e.target.value as HardwareAcceleration)}
                    >
                      <option value="auto">Auto</option>
                      <option value="none">Off</option>
                      <option value="nvenc">NVENC (NVIDIA)</option>
                      <option value="amf">AMF (AMD)</option>
                      <option value="qsv">Quick Sync (Intel)</option>
                    </select>
                  </div>
                </>
              )}

              {hasAudioTrack && (
                <div className={styles.field}>
                  <label className={styles.fieldLabel} htmlFor="audioBitrate">
                    Audio bitrate (kbps)
                  </label>
                  <input
                    id="audioBitrate"
                    className={styles.numberInput}
                    type="number"
                    placeholder="auto"
                    value={audioBitrateKbps}
                    onChange={(e) => setAudioBitrateKbps(e.target.value)}
                  />
                </div>
              )}

              <div className={styles.field}>
                <label className={styles.fieldLabel} htmlFor="outputDirectory">
                  Output folder
                </label>
                <input
                  id="outputDirectory"
                  className={styles.textInput}
                  type="text"
                  placeholder="D:\Converted"
                  value={outputDirectory}
                  onChange={(e) => setOutputDirectory(e.target.value)}
                />
              </div>

              <div className={styles.footer}>
                <button className={styles.submitButton} onClick={() => void handleSubmit()} disabled={!canSubmit}>
                  {creating
                    ? `Starting ${readyItems.length}...`
                    : `${mode === "convert" ? "Convert" : "Compress"} ${readyItems.length} file${
                        readyItems.length === 1 ? "" : "s"
                      }`}
                </button>
              </div>
              <ErrorBanner error={createError} />
            </GlassCard>
          )}

          {submitted && (
            <GlassCard className={styles.section}>
              <div className={styles.fileRow}>
                <div className={styles.fileName}>
                  {completedCount} of {submittedItems.length} done
                  {failedCount > 0 ? ` \u00b7 ${failedCount} failed` : ""}
                  {cancelledCount > 0 ? ` \u00b7 ${cancelledCount} cancelled` : ""}
                </div>
                <span className={styles.stateBadge}>{batchInFlight ? "RUNNING" : "FINISHED"}</span>
              </div>
              <div className={styles.progressTrack}>
                <div
                  className={styles.progressFill}
                  style={{
                    width: `${
                      submittedItems.length === 0
                        ? 0
                        : ((completedCount + failedCount + cancelledCount) / submittedItems.length) * 100
                    }%`,
                  }}
                />
              </div>
              <ErrorBanner error={createError} />

              <div className={styles.footer}>
                {batchInFlight && (
                  <button className={styles.linkButton} onClick={() => void handleCancelAll()}>
                    Cancel remaining ({activeJobs.length})
                  </button>
                )}
                {firstOutputPath && (
                  <button className={styles.submitButton} onClick={() => void handleOpenFolder()}>
                    Open folder
                  </button>
                )}
                {!batchInFlight && (
                  <button className={styles.linkButton} onClick={handleStartOver}>
                    Convert more
                  </button>
                )}
                <button className={styles.linkButton} onClick={() => navigate({ kind: "queue" })}>
                  View queue
                </button>
              </div>
              <ErrorBanner error={openFolderError} />
            </GlassCard>
          )}
        </>
      )}
    </div>
  );
}

// Offered a path that did not inspect as a convertible file. Resolves to the folder's
// convertible contents, or null if it simply is not a folder -- "you dropped a .txt" and
// "you dropped a folder" must not be the same answer.
async function tryListFolder(
  path: string,
): Promise<{ kind: "folder"; files: FileInfo[]; skipped: number; truncated: boolean } | null> {
  try {
    const { files, skipped, truncated } = await coreClient.listFolderFiles(path);
    return { kind: "folder", files, skipped, truncated };
  } catch {
    return null;
  }
}
