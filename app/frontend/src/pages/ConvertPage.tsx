import { useCallback, useEffect, useMemo, useState, type DragEvent } from "react";
import GlassCard from "../components/GlassCard";
import PresetBar from "../components/PresetBar";
import { ProLockedControl } from "../components/ProLockedBadge";
import { useJobs } from "../hooks/useJobs";
import { useNavigation } from "../navigation/NavigationContext";
import * as coreClient from "../services/coreClient";
import type { FileCategory, FileInfo } from "../types/fileInfo";
import type { ErrorInfo } from "../types/error";
import type { HardwareAcceleration, MediaProcessingOptions, MediaQuality, VideoCodec } from "../types/conversion";
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

function extractDroppedPath(files: FileList): string | undefined {
  const first = files[0];
  // Tauri's webview exposes a real filesystem path via `.path` on a dropped File -- same
  // extraction HomePage.tsx already relies on for its own drop targets.
  return (first as File & { path?: string }).path;
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
  const [inputPath, setInputPath] = useState<string | null>(prefill?.prefillFilePath ?? null);
  const [fileInfo, setFileInfo] = useState<FileInfo | null>(null);
  const [inspecting, setInspecting] = useState(false);
  const [inspectError, setInspectError] = useState<ErrorInfo | null>(null);
  const [dropActive, setDropActive] = useState(false);

  const [outputFormat, setOutputFormat] = useState("");
  const [quality, setQuality] = useState<MediaQuality>("medium");
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
  // hand for the common case -- they can still override it per job.
  useEffect(() => {
    coreClient
      .getSettings()
      .then(({ settings }) => setOutputDirectory(settings.general.defaultOutputDirectory))
      .catch(() => {
        // Non-fatal -- the field just starts empty and the user fills it in.
      });
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

  const handleDrop = useCallback((event: DragEvent<HTMLDivElement>) => {
    event.preventDefault();
    setDropActive(false);
    const path = extractDroppedPath(event.dataTransfer.files);
    if (path) setInputPath(path);
  }, []);

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
    if (opts.quality && opts.quality !== "lossless") setQuality(opts.quality);
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
          className={`${styles.dropzone} ${dropActive ? styles.dropzoneActive : ""}`}
          ariaLabel="Drop a file to convert or compress"
        >
          <div
            className={styles.dropzoneInner}
            onDragOver={(e) => {
              e.preventDefault();
              setDropActive(true);
            }}
            onDragLeave={() => setDropActive(false)}
            onDrop={handleDrop}
          >
            <div className={styles.dropIcon}>&#8646;</div>
            <p>Drop a file here to get started.</p>
            <ProLockedControl label="Select multiple files for batch conversion">
              <span className={styles.batchStub}>Batch convert</span>
            </ProLockedControl>
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
                <div className={styles.qualityRow}>
                  <select className={styles.selectInput} value={quality} onChange={(e) => setQuality(e.target.value as MediaQuality)}>
                    <option value="low">Low</option>
                    <option value="medium">Medium</option>
                    <option value="high">High</option>
                  </select>
                  <ProLockedControl label="Lossless (LZMA2)">
                    <select className={styles.selectInput} disabled>
                      <option>Lossless</option>
                    </select>
                  </ProLockedControl>
                </div>
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
                  <div className={styles.field}>
                    <label className={styles.fieldLabel}>Trim</label>
                    <ProLockedControl label="Trim before converting">
                      <span className={styles.trimStub}>00:00:00 &ndash; 00:00:00</span>
                    </ProLockedControl>
                  </div>
                  <div className={styles.field}>
                    <label className={styles.fieldLabel}>Watermark</label>
                    <ProLockedControl label="Watermark overlay">
                      <span className={styles.trimStub}>No image selected</span>
                    </ProLockedControl>
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
