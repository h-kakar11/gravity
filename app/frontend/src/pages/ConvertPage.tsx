import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { open as openFilePicker } from "@tauri-apps/plugin-dialog";
import GlassCard from "../components/GlassCard";
import PresetBar from "../components/PresetBar";
import { useJobs } from "../hooks/useJobs";
import { useNavigation } from "../navigation/NavigationContext";
import * as coreClient from "../services/coreClient";
import type { FileCategory, FileInfo } from "../types/fileInfo";
import type { ErrorInfo } from "../types/error";
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

// Real Convert/Compress screen (Phase 2.6's engine finally gets a page): drop or prefill a
// local file -> pick an output format/quality -> real ffmpeg job -> live progress ->
// verified completion. Convert and Compress are the same form submitting to different
// createJob types (docs/decisions.md: Compress is Convert with different default option
// values, not a different code path) -- so this is one component with a mode toggle, not two.

type Mode = "convert" | "compress";

const FORMAT_OPTIONS_BY_CATEGORY: Record<FileCategory, string[]> = {
  VIDEO: ["mp4", "webm", "mov", "mkv", "gif"],
  AUDIO: ["mp3", "wav", "flac", "aac", "m4a", "ogg"],
  IMAGE: ["png", "jpg", "webp", "gif", "bmp"],
  DOCUMENT: [],
  TEXT: [],
  ARCHIVE: [],
  UNKNOWN: ["mp4", "mp3", "png"],
};

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
  const [inputPath, setInputPath] = useState<string | null>(prefill?.prefillFilePath ?? null);
  const [fileInfo, setFileInfo] = useState<FileInfo | null>(null);
  const [inspecting, setInspecting] = useState(false);
  const [inspectError, setInspectError] = useState<ErrorInfo | null>(null);

  const [outputFormat, setOutputFormat] = useState("");
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
  const [activeJobId, setActiveJobId] = useState<string | null>(null);
  const [openFolderError, setOpenFolderError] = useState<ErrorInfo | null>(null);

  const { jobs, cancelJob } = useJobs();
  const activeJob = useMemo(() => jobs.find((j) => j.id === activeJobId) ?? null, [jobs, activeJobId]);
  const jobInFlight = activeJob !== null && !["COMPLETED", "FAILED", "CANCELLED"].includes(activeJob.state);

  // Seed the output directory from Settings once, so the user isn't stuck typing a path by
  // hand for the common case -- they can still override it per job. Same for the default
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

  useEffect(() => {
    if (!inputPath) {
      setFileInfo(null);
      return;
    }
    let active = true;
    setInspecting(true);
    setInspectError(null);
    coreClient
      .inspectFile(inputPath)
      .then(({ fileInfo: info }) => {
        if (!active) return;
        setFileInfo(info);
        const options = FORMAT_OPTIONS_BY_CATEGORY[info.category];
        if (options.length > 0) setOutputFormat(options[0]);
      })
      .catch((err) => {
        if (active) setInspectError(asErrorInfo(err));
      })
      .finally(() => {
        if (active) setInspecting(false);
      });
    return () => {
      active = false;
    };
  }, [inputPath]);

  const formatOptions = fileInfo ? FORMAT_OPTIONS_BY_CATEGORY[fileInfo.category] : [];
  const isVideo = fileInfo?.category === "VIDEO";
  const hasAudio = fileInfo?.category === "VIDEO" || fileInfo?.category === "AUDIO";
  const canSubmit =
    inputPath !== null && outputFormat.trim().length > 0 && outputDirectory.trim().length > 0 && !creating && !jobInFlight;

  // Plain function (not memoized): both handleSubmit and PresetBar's "save as preset"
  // button need the options object built from whatever the form currently holds, and
  // PresetBar re-renders alongside this component on every state change anyway.
  const buildOptions = (): MediaProcessingOptions => ({
    outputFormat,
    quality,
    ...(isVideo ? { videoCodec, hardwareAcceleration } : {}),
    ...(hasAudio && audioBitrateKbps.trim() ? { audioBitrateKbps: Number(audioBitrateKbps) } : {}),
  });

  const applyPresetOptions = useCallback((options: Record<string, unknown>) => {
    const opts = options as Partial<MediaProcessingOptions>;
    if (typeof opts.outputFormat === "string") setOutputFormat(opts.outputFormat);
    if (opts.quality) {
      qualityTouched.current = true;
      setQuality(opts.quality);
    }
    if (opts.videoCodec) setVideoCodec(opts.videoCodec);
    if (opts.hardwareAcceleration) setHardwareAcceleration(opts.hardwareAcceleration);
    if (typeof opts.audioBitrateKbps === "number") setAudioBitrateKbps(String(opts.audioBitrateKbps));
  }, []);

  const handleSubmit = useCallback(async () => {
    if (!inputPath || !outputFormat) return;
    setCreating(true);
    setCreateError(null);
    try {
      const options = buildOptions();
      const params = { inputPath, outputDirectory: outputDirectory.trim(), options };
      const { jobId } = mode === "convert" ? await coreClient.createConversionJob(params) : await coreClient.createCompressionJob(params);
      setActiveJobId(jobId);
    } catch (err) {
      setCreateError(asErrorInfo(err));
    } finally {
      setCreating(false);
    }
  }, [inputPath, outputFormat, quality, isVideo, videoCodec, hardwareAcceleration, hasAudio, audioBitrateKbps, outputDirectory, mode]);

  const handleCancel = useCallback(async () => {
    if (!activeJobId) return;
    await cancelJob(activeJobId);
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
    setInputPath(null);
    setFileInfo(null);
    setCreateError(null);
  }, []);

  // File-picker fallback alongside drag-and-drop (issue #57: "should have a selection of
  // what file to convert rather than relying on solely drag and drop").
  const handleBrowseFile = useCallback(async () => {
    const selected = await openFilePicker({
      multiple: false,
      title: "Choose a file to convert or compress",
    });
    if (typeof selected === "string") setInputPath(selected);
  }, []);

  return (
    <div className={styles.wrap}>
      <div className={styles.header}>
        <h1 className={styles.title}>Convert &amp; Compress</h1>
        <div className={styles.modeToggle}>
          <button
            className={`${styles.modeButton} ${mode === "convert" ? styles.modeButtonActive : ""}`}
            onClick={() => setMode("convert")}
            disabled={jobInFlight}
          >
            Convert
          </button>
          <button
            className={`${styles.modeButton} ${mode === "compress" ? styles.modeButtonActive : ""}`}
            onClick={() => setMode("compress")}
            disabled={jobInFlight}
          >
            Compress
          </button>
        </div>
      </div>

      {!inputPath ? (
        <GlassCard
          className={styles.dropzone}
          ariaLabel="Drop a file to convert or compress"
          onFilesDropped={(paths) => {
            if (paths[0]) setInputPath(paths[0]);
          }}
        >
          <div className={styles.dropzoneInner}>
            <div className={styles.dropIcon}>&#8646;</div>
            <p>Drop a file here to get started.</p>
            <button
              className={styles.linkButton}
              onClick={(e) => {
                e.stopPropagation();
                void handleBrowseFile();
              }}
            >
              or choose a file...
            </button>
          </div>
        </GlassCard>
      ) : (
        <>
          <GlassCard className={styles.section}>
            <div className={styles.fileRow}>
              <div>
                <div className={styles.fileName}>{fileInfo?.filename ?? inputPath}</div>
                {fileInfo && (
                  <div className={styles.fileMeta}>
                    {fileInfo.category} &middot; {formatBytes(fileInfo.sizeBytes)}
                    {fileInfo.width && fileInfo.height ? ` \u00b7 ${fileInfo.width}\u00d7${fileInfo.height}` : ""}
                  </div>
                )}
                {inspecting && <div className={styles.fileMeta}>Inspecting...</div>}
              </div>
              {!jobInFlight && (
                <button className={styles.linkButton} onClick={handleStartOver}>
                  Choose a different file
                </button>
              )}
            </div>
            <ErrorBanner error={inspectError} />
          </GlassCard>

          {fileInfo && !jobInFlight && !activeJob && (
            <GlassCard className={styles.section}>
              <PresetBar
                kind={mode === "convert" ? "CONVERSION" : "COMPRESSION"}
                currentOptions={() => buildOptions() as unknown as Record<string, unknown>}
                onApply={applyPresetOptions}
              />
              <div className={styles.field}>
                <label className={styles.fieldLabel}>Output format</label>
                <select className={styles.selectInput} value={outputFormat} onChange={(e) => setOutputFormat(e.target.value)}>
                  {formatOptions.map((fmt) => (
                    <option key={fmt} value={fmt}>
                      .{fmt}
                    </option>
                  ))}
                </select>
              </div>

              <div className={styles.field}>
                <label className={styles.fieldLabel}>Quality</label>
                <select className={styles.selectInput} value={quality} onChange={(e) => chooseQuality(e.target.value as MediaQuality)}>
                  {qualityTiers.map((tier) => (
                    <option key={tier} value={tier}>
                      {QUALITY_LABELS[tier]}
                    </option>
                  ))}
                </select>
              </div>

              {isVideo && (
                <>
                  <div className={styles.field}>
                    <label className={styles.fieldLabel}>Video codec</label>
                    <select className={styles.selectInput} value={videoCodec} onChange={(e) => setVideoCodec(e.target.value as VideoCodec)}>
                      <option value="auto">Auto</option>
                      <option value="h264">H.264</option>
                      <option value="h265">H.265</option>
                      <option value="vp9">VP9</option>
                      <option value="av1">AV1</option>
                    </select>
                  </div>
                  <div className={styles.field}>
                    <label className={styles.fieldLabel}>Hardware acceleration</label>
                    <select
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

              {hasAudio && (
                <div className={styles.field}>
                  <label className={styles.fieldLabel}>Audio bitrate (kbps)</label>
                  <input
                    className={styles.numberInput}
                    type="number"
                    placeholder="auto"
                    value={audioBitrateKbps}
                    onChange={(e) => setAudioBitrateKbps(e.target.value)}
                  />
                </div>
              )}

              <div className={styles.field}>
                <label className={styles.fieldLabel}>Output folder</label>
                <input
                  className={styles.textInput}
                  type="text"
                  placeholder="D:\Converted"
                  value={outputDirectory}
                  onChange={(e) => setOutputDirectory(e.target.value)}
                />
              </div>

              <div className={styles.footer}>
                <button className={styles.submitButton} onClick={() => void handleSubmit()} disabled={!canSubmit}>
                  {creating ? "Starting..." : mode === "convert" ? "Convert" : "Compress"}
                </button>
              </div>
              <ErrorBanner error={createError} />
            </GlassCard>
          )}

          {activeJob && (
            <GlassCard className={styles.section}>
              <div className={styles.fileRow}>
                <div className={styles.fileName}>{activeJob.progress.statusMessage}</div>
                <span className={styles.stateBadge}>{activeJob.state}</span>
              </div>
              {activeJob.progress.percentage !== undefined && (
                <div className={styles.progressTrack}>
                  <div className={styles.progressFill} style={{ width: `${activeJob.progress.percentage}%` }} />
                </div>
              )}
              <div className={styles.metaRow}>
                {formatBytesPerSecond(activeJob.progress.speedBytesPerSecond) && (
                  <span>{formatBytesPerSecond(activeJob.progress.speedBytesPerSecond)}</span>
                )}
                {formatEta(activeJob.progress.etaSeconds) && <span>{formatEta(activeJob.progress.etaSeconds)}</span>}
              </div>

              {jobInFlight && (
                <div className={styles.footer}>
                  <button className={styles.linkButton} onClick={() => void handleCancel()}>
                    Cancel
                  </button>
                </div>
              )}

              {activeJob.state === "COMPLETED" && (
                <div className={styles.footer}>
                  <button className={styles.submitButton} onClick={() => void handleOpenFolder()}>
                    Open folder
                  </button>
                  <button className={styles.linkButton} onClick={handleStartOver}>
                    Convert another
                  </button>
                  <button className={styles.linkButton} onClick={() => navigate({ kind: "queue" })}>
                    View queue
                  </button>
                </div>
              )}
              <ErrorBanner error={openFolderError} />

              {activeJob.state === "FAILED" && activeJob.error && (
                <>
                  <ErrorBanner error={activeJob.error} />
                  <div className={styles.footer}>
                    <button className={styles.linkButton} onClick={handleStartOver}>
                      Try again
                    </button>
                  </div>
                </>
              )}

              {activeJob.state === "CANCELLED" && (
                <div className={styles.footer}>
                  <button className={styles.linkButton} onClick={handleStartOver}>
                    Start over
                  </button>
                </div>
              )}
            </GlassCard>
          )}
        </>
      )}
    </div>
  );
}
