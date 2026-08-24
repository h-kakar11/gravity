// Creates conversion and compression jobs, and the download -> convert -> compress pipeline
// that the dependency model exists for (spec section 19).
//
// The pipeline is built by the BACKEND, not here. This page submits the second job naming
// the first via `inputFromJobId`; the backend links them, decides when each may start, and
// resolves the real input path once the producer has actually run. The frontend never polls
// for a job to finish and then submits the next one, and -- just as importantly -- never
// guesses what path a job will produce.

import { useCallback, useEffect, useState } from "react";

import * as coreClient from "../services/coreClient";
import type { ErrorInfo } from "../types/error";
import type { JobPriority } from "../types/job";
import type { CompressionPreset, TargetFormat } from "../types/ipc";
import { asErrorInfo } from "../utils/errors";
import { describeError } from "../utils/jobDisplay";
import { AlertTriangleIcon, CheckCircleIcon, CompressIcon, ConvertIcon } from "../components/icons";
import { Button } from "../components/ui/Button";

type Mode = "CONVERT" | "COMPRESS" | "CONVERT_THEN_COMPRESS";

const MODES: { id: Mode; label: string; icon: React.ComponentType<{ size?: number }>; hint: string }[] = [
  { id: "CONVERT", label: "Convert", icon: ConvertIcon, hint: "Change the file's format." },
  { id: "COMPRESS", label: "Compress", icon: CompressIcon, hint: "Re-encode smaller, same format." },
  {
    id: "CONVERT_THEN_COMPRESS",
    label: "Convert, then compress",
    icon: ConvertIcon,
    hint: "Two linked jobs. The second waits for the first and uses its output.",
  },
];

// Plain-language presets rather than raw FFmpeg terminology (spec section 8). Wire values
// stay LOW/MEDIUM/HIGH -- only the words shown to the user change.
const PRESET_OPTIONS: { id: CompressionPreset; label: string; hint: string }[] = [
  { id: "LOW", label: "Smaller file", hint: "Most compression, some quality loss." },
  { id: "MEDIUM", label: "Balanced", hint: "A middle ground between size and quality." },
  { id: "HIGH", label: "High quality", hint: "Largest file, least quality loss." },
];

export default function ProcessPage() {
  const [mode, setMode] = useState<Mode>("CONVERT");
  const [inputPath, setInputPath] = useState("");
  const [outputDirectory, setOutputDirectory] = useState("");
  const [targetFormat, setTargetFormat] = useState<TargetFormat>("MP4");
  const [preset, setPreset] = useState<CompressionPreset>("MEDIUM");
  const [maxHeight, setMaxHeight] = useState<string>("");
  const [priority, setPriority] = useState<JobPriority>("NORMAL");

  const [formats, setFormats] = useState<TargetFormat[]>([]);
  const [ffmpegAvailable, setFfmpegAvailable] = useState<boolean | null>(null);
  const [error, setError] = useState<ErrorInfo | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  const [submitting, setSubmitting] = useState(false);

  // The format list comes from the backend rather than a hardcoded copy, so a format the
  // core does not actually implement can never appear in this picker.
  useEffect(() => {
    let active = true;
    coreClient
      .getProcessingCapabilities()
      .then((caps) => {
        if (!active) return;
        setFormats(caps.targetFormats);
        setFfmpegAvailable(caps.ffmpegAvailable);
      })
      .catch((err) => {
        if (active) setError(asErrorInfo(err));
      });
    return () => {
      active = false;
    };
  }, []);

  const parsedMaxHeight = maxHeight.trim() === "" ? undefined : Number(maxHeight);
  const maxHeightInvalid =
    parsedMaxHeight !== undefined && (!Number.isInteger(parsedMaxHeight) || parsedMaxHeight < 16);
  const canSubmit =
    inputPath.trim() !== "" && outputDirectory.trim() !== "" && !maxHeightInvalid && !submitting;

  const submit = useCallback(async () => {
    setSubmitting(true);
    setError(null);
    setNotice(null);
    try {
      const common = { inputPath: inputPath.trim(), outputDirectory: outputDirectory.trim() };

      if (mode === "COMPRESS") {
        const { jobId } = await coreClient.createCompressionJob(
          { ...common, preset, maxHeight: parsedMaxHeight },
          { priority },
        );
        setNotice(`Compression queued (${jobId}). Watch it on the Queue page.`);
        return;
      }

      const conversion = await coreClient.createConversionJob(
        { ...common, targetFormat, maxHeight: parsedMaxHeight },
        { priority },
      );

      if (mode === "CONVERT") {
        setNotice(`Conversion queued (${conversion.jobId}). Watch it on the Queue page.`);
        return;
      }

      // The compression names the conversion rather than a path. Nothing here guesses what
      // the conversion will produce, and nothing polls for it to finish: the backend
      // resolves the real path once that job has completed, and the declared link means the
      // compression cannot start before then -- or at all, if the conversion fails, in which
      // case it is marked SKIPPED rather than run against a missing file.
      const stem = (common.inputPath.split(/[\\/]/).pop() ?? "output").replace(/\.[^.]*$/, "");
      const compression = await coreClient.createCompressionJob(
        {
          inputFromJobId: conversion.jobId,
          outputDirectory: common.outputDirectory,
          preset,
          outputFilenameBase: `${stem}-compressed`,
          outputExtension: "mp4",
        },
        { priority, parentJobId: conversion.jobId },
      );
      setNotice(
        `Queued 2 linked jobs: convert (${conversion.jobId}), then compress (${compression.jobId}).`,
      );
    } catch (err) {
      setError(asErrorInfo(err));
    } finally {
      setSubmitting(false);
    }
  }, [mode, inputPath, outputDirectory, targetFormat, preset, parsedMaxHeight, priority]);

  const showFormat = mode !== "COMPRESS";
  const showPreset = mode !== "CONVERT";

  return (
    <div className="gv-enter">
      <h1 className="gv-h1">Convert &amp; Compress</h1>
      <p className="gv-subtitle">Local files only. Everything queued here joins the same queue as downloads.</p>

      {ffmpegAvailable === false ? (
        <div className="gv-banner gv-banner--warning" role="alert">
          <AlertTriangleIcon size={15} />
          <div className="gv-banner__body">
            <div className="gv-banner__title">FFmpeg not found</div>
            <div className="gv-banner__detail">
              Conversion and compression can't run until FFmpeg is installed, or its path is set
              in Settings.
            </div>
          </div>
        </div>
      ) : null}

      {error ? (
        <div className="gv-banner gv-banner--error" role="alert">
          <AlertTriangleIcon size={15} />
          <div className="gv-banner__body">
            <div className="gv-banner__title">{describeError(error)}</div>
            {error.details && error.details !== error.message ? (
              <div className="gv-banner__detail">{error.details}</div>
            ) : null}
          </div>
        </div>
      ) : null}

      {notice ? (
        <div className="gv-banner gv-banner--success" role="status">
          <CheckCircleIcon size={15} />
          <div className="gv-banner__body">
            <div className="gv-banner__title">{notice}</div>
          </div>
        </div>
      ) : null}

      <div className="gv-tabs" role="tablist" aria-label="Operation">
        {MODES.map(({ id, label, icon: Glyph, hint }) => (
          <button
            key={id}
            type="button"
            role="tab"
            aria-selected={mode === id}
            className="gv-tab"
            title={hint}
            onClick={() => setMode(id)}
          >
            <span style={{ display: "inline-flex", alignItems: "center", gap: "0.35rem" }}>
              <Glyph size={14} />
              {label}
            </span>
          </button>
        ))}
      </div>
      <p className="gv-subtitle" style={{ marginTop: "-0.5rem" }}>
        {MODES.find((m) => m.id === mode)?.hint}
      </p>

      <div className="gv-panel" style={{ display: "grid", gap: "1rem", maxWidth: 640 }}>
        <div className="gv-field">
          <label className="gv-label" htmlFor="inputPath">
            Input file
          </label>
          <input
            id="inputPath"
            className="gv-input"
            value={inputPath}
            onChange={(e) => setInputPath(e.target.value)}
            placeholder="Choose a local file"
          />
        </div>

        <div className="gv-field">
          <label className="gv-label" htmlFor="outputDirectory">
            Output folder
          </label>
          <input
            id="outputDirectory"
            className="gv-input"
            value={outputDirectory}
            onChange={(e) => setOutputDirectory(e.target.value)}
            placeholder="Choose a destination folder"
          />
        </div>

        {showFormat ? (
          <div className="gv-field">
            <label className="gv-label" htmlFor="targetFormat">
              Convert to
            </label>
            <select
              id="targetFormat"
              className="gv-select"
              value={targetFormat}
              onChange={(e) => setTargetFormat(e.target.value as TargetFormat)}
            >
              {(formats.length > 0 ? formats : [targetFormat]).map((value) => (
                <option key={value} value={value}>
                  {value}
                </option>
              ))}
            </select>
          </div>
        ) : null}

        {showPreset ? (
          <div className="gv-field">
            <label className="gv-label" htmlFor="preset">
              Compression quality
            </label>
            <select
              id="preset"
              className="gv-select"
              value={preset}
              onChange={(e) => setPreset(e.target.value as CompressionPreset)}
            >
              {PRESET_OPTIONS.map(({ id, label }) => (
                <option key={id} value={id}>
                  {label}
                </option>
              ))}
            </select>
            <span className="gv-hint">{PRESET_OPTIONS.find((p) => p.id === preset)?.hint}</span>
          </div>
        ) : null}

        <div className="gv-field">
          <label className="gv-label" htmlFor="maxHeight">
            Maximum height in pixels (optional)
          </label>
          <input
            id="maxHeight"
            className="gv-input"
            value={maxHeight}
            inputMode="numeric"
            onChange={(e) => setMaxHeight(e.target.value)}
            placeholder="e.g. 720 — leave empty to keep the source size"
            aria-invalid={maxHeightInvalid}
            aria-describedby={maxHeightInvalid ? "maxHeightError" : undefined}
          />
          {maxHeightInvalid ? (
            <span id="maxHeightError" className="gv-hint" style={{ color: "var(--status-failed)" }}>
              Enter a whole number of 16 or more, or leave it empty.
            </span>
          ) : null}
        </div>

        <div className="gv-field">
          <label className="gv-label" htmlFor="jobPriority">
            Priority
          </label>
          <select
            id="jobPriority"
            className="gv-select"
            value={priority}
            onChange={(e) => setPriority(e.target.value as JobPriority)}
          >
            <option value="LOW">Low</option>
            <option value="NORMAL">Normal</option>
            <option value="HIGH">High</option>
          </select>
        </div>

        <div>
          <Button variant="primary" busy={submitting} disabled={!canSubmit} onClick={() => void submit()}>
            Add to queue
          </Button>
        </div>
      </div>
    </div>
  );
}
