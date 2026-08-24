// Creates conversion and compression jobs, and the download -> convert -> compress pipeline
// that the dependency model exists for (spec section 19).
//
// The pipeline is built by the BACKEND, not here: this page submits the jobs together with
// a `dependsOn` link and the backend decides when each may start. The frontend never polls
// for a job to finish and then submits the next one -- that would be exactly the
// "manually poll and construct a second unrelated operation" design the spec rules out.

import { useCallback, useEffect, useState } from "react";

import { styles } from "../components/queueStyles";
import * as coreClient from "../services/coreClient";
import type { ErrorInfo } from "../types/error";
import type { JobPriority } from "../types/job";
import type { CompressionPreset, TargetFormat } from "../types/ipc";
import { asErrorInfo } from "../utils/errors";
import { describeError } from "../utils/jobDisplay";

type Mode = "CONVERT" | "COMPRESS" | "CONVERT_THEN_COMPRESS";

const MODES: { id: Mode; label: string; hint: string }[] = [
  { id: "CONVERT", label: "Convert", hint: "Change the file's format." },
  { id: "COMPRESS", label: "Compress", hint: "Re-encode smaller, same format." },
  {
    id: "CONVERT_THEN_COMPRESS",
    label: "Convert, then compress",
    hint: "Two linked jobs. The second waits for the first and uses its output.",
  },
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
        setNotice(`Compression queued (${jobId}). Watch it on the Queue tab.`);
        return;
      }

      const conversion = await coreClient.createConversionJob(
        { ...common, targetFormat, maxHeight: parsedMaxHeight },
        { priority },
      );

      if (mode === "CONVERT") {
        setNotice(`Conversion queued (${conversion.jobId}). Watch it on the Queue tab.`);
        return;
      }

      // The compression's input is the conversion's output path, which does not exist yet.
      // That is fine: the dependency means the backend will not start it until the
      // conversion has completed successfully, and if the conversion fails the compression
      // is marked SKIPPED rather than run against a missing file.
      const stem = common.inputPath.split(/[\\/]/).pop()?.replace(/\.[^.]*$/, "") ?? "output";
      const separator = common.outputDirectory.includes("\\") ? "\\" : "/";
      const convertedPath = `${common.outputDirectory}${separator}${stem}.${targetFormat.toLowerCase()}`;

      const compression = await coreClient.createCompressionJob(
        {
          inputPath: convertedPath,
          outputDirectory: common.outputDirectory,
          preset,
          outputFilenameBase: `${stem}-compressed`,
        },
        { priority, dependsOn: [conversion.jobId], parentJobId: conversion.jobId },
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
    <div style={styles.page}>
      <h1 style={styles.h1}>Convert &amp; Compress</h1>
      <p style={styles.subtitle}>
        Local files only. Everything queued here joins the same queue as downloads.
      </p>

      {ffmpegAvailable === false ? (
        <div style={styles.errorBanner} role="alert">
          FFmpeg was not found on this system, so conversion and compression cannot run. Install
          it, or set its path under Advanced settings.
        </div>
      ) : null}

      {error ? (
        <div style={styles.errorBanner} role="alert">
          <strong>{describeError(error)}</strong>
          {error.details && error.details !== error.message ? (
            <div style={{ fontSize: "0.78rem", marginTop: "0.25rem" }}>{error.details}</div>
          ) : null}
        </div>
      ) : null}

      {notice ? (
        <div
          style={{ ...styles.errorBanner, borderColor: "#86efac", background: "#f0fdf4", color: "#166534" }}
          role="status"
        >
          {notice}
        </div>
      ) : null}

      <div style={styles.filterBar} role="tablist" aria-label="Operation">
        {MODES.map(({ id, label }) => (
          <button
            key={id}
            type="button"
            role="tab"
            aria-selected={mode === id}
            style={mode === id ? styles.filterTabActive : styles.filterTab}
            onClick={() => setMode(id)}
          >
            {label}
          </button>
        ))}
      </div>
      <p style={styles.subtitle}>{MODES.find((m) => m.id === mode)?.hint}</p>

      <div style={{ display: "grid", gap: "0.75rem", maxWidth: 640 }}>
        <div>
          <label htmlFor="inputPath" style={styles.toolbarLabel}>
            Input file
          </label>
          <input
            id="inputPath"
            value={inputPath}
            onChange={(e) => setInputPath(e.target.value)}
            placeholder="C:\Users\you\Videos\clip.mp4"
            style={{ ...styles.button, width: "100%", cursor: "text" }}
          />
        </div>

        <div>
          <label htmlFor="outputDirectory" style={styles.toolbarLabel}>
            Output folder
          </label>
          <input
            id="outputDirectory"
            value={outputDirectory}
            onChange={(e) => setOutputDirectory(e.target.value)}
            placeholder="C:\Users\you\Videos\out"
            style={{ ...styles.button, width: "100%", cursor: "text" }}
          />
        </div>

        {showFormat ? (
          <div>
            <label htmlFor="targetFormat" style={styles.toolbarLabel}>
              Convert to
            </label>
            <select
              id="targetFormat"
              value={targetFormat}
              onChange={(e) => setTargetFormat(e.target.value as TargetFormat)}
              style={{ ...styles.button, width: "100%" }}
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
          <div>
            <label htmlFor="preset" style={styles.toolbarLabel}>
              Compression quality
            </label>
            <select
              id="preset"
              value={preset}
              onChange={(e) => setPreset(e.target.value as CompressionPreset)}
              style={{ ...styles.button, width: "100%" }}
            >
              <option value="LOW">Low — smallest file</option>
              <option value="MEDIUM">Medium — balanced</option>
              <option value="HIGH">High — best quality</option>
            </select>
          </div>
        ) : null}

        <div>
          <label htmlFor="maxHeight" style={styles.toolbarLabel}>
            Maximum height in pixels (optional)
          </label>
          <input
            id="maxHeight"
            value={maxHeight}
            inputMode="numeric"
            onChange={(e) => setMaxHeight(e.target.value)}
            placeholder="e.g. 720 — leave empty to keep the source size"
            aria-invalid={maxHeightInvalid}
            aria-describedby={maxHeightInvalid ? "maxHeightError" : undefined}
            style={{
              ...styles.button,
              width: "100%",
              cursor: "text",
              borderColor: maxHeightInvalid ? "#b91c1c" : "#ccc",
            }}
          />
          {maxHeightInvalid ? (
            <div id="maxHeightError" style={{ color: "#b91c1c", fontSize: "0.78rem" }}>
              Enter a whole number of 16 or more, or leave it empty.
            </div>
          ) : null}
        </div>

        <div>
          <label htmlFor="jobPriority" style={styles.toolbarLabel}>
            Priority
          </label>
          <select
            id="jobPriority"
            value={priority}
            onChange={(e) => setPriority(e.target.value as JobPriority)}
            style={{ ...styles.button, width: "100%" }}
          >
            <option value="LOW">Low</option>
            <option value="NORMAL">Normal</option>
            <option value="HIGH">High</option>
          </select>
        </div>

        <div>
          <button
            type="button"
            disabled={!canSubmit}
            onClick={() => void submit()}
            style={canSubmit ? styles.buttonPrimary : { ...styles.buttonPrimary, ...styles.buttonDisabled }}
          >
            {submitting ? "Queueing…" : "Add to queue"}
          </button>
        </div>
      </div>
    </div>
  );
}
