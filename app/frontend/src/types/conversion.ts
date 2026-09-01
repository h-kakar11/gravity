// Mirrors engines/ffmpeg/FFmpegArgBuilder.h's MediaProcessingOptions and
// core/jobs/MediaProcessingJob.h's Options -- the params shape for a CONVERSION or
// COMPRESSION createJob call (docs/ipc-contract.md).

// The same tier vocabulary core/settings/Settings.h uses for
// processing.defaultCompressionQuality, plus "lossless" (issue #83: the Convert/Compress
// page offered only low/medium/high, so the two extremes Settings advertised could not be
// reached from the page that actually runs a job).
export type MediaQuality = "lowest" | "low" | "medium" | "high" | "ultra" | "lossless";

// Selectable tiers, worst-to-best. Compression never offers "lossless": a compression job
// sizes its output as a fraction of the source (see MediaProcessingJob), and "make it
// smaller, losslessly, by re-encoding" is not a request ffmpeg can honour.
export const LOSSY_QUALITY_TIERS: MediaQuality[] = ["lowest", "low", "medium", "high", "ultra"];
export const CONVERSION_QUALITY_TIERS: MediaQuality[] = [...LOSSY_QUALITY_TIERS, "lossless"];

export const QUALITY_LABELS: Record<MediaQuality, string> = {
  lowest: "Lowest (smallest file)",
  low: "Low",
  medium: "Medium",
  high: "High",
  ultra: "Ultra (largest file)",
  lossless: "Lossless",
};
export type VideoCodec = "auto" | "h264" | "h265" | "vp9" | "av1";
export type HardwareAcceleration = "auto" | "none" | "nvenc" | "amf" | "qsv";
export type WatermarkPosition = "top-left" | "top-right" | "bottom-left" | "bottom-right" | "center";

export interface WatermarkOptions {
  imagePath: string;
  position: WatermarkPosition;
  opacity: number; // 0.0-1.0
}

export interface MediaProcessingOptions {
  outputFormat: string; // "mp4" | "webm" | "mov" | "gif" | "mp3" | "webp" | ... (required)
  // Validated against the tier list above at the IPC boundary (main.cpp) -- an
  // unrecognized value is rejected rather than silently degraded to "medium".
  quality?: MediaQuality;
  videoCodec?: VideoCodec;
  hardwareAcceleration?: HardwareAcceleration;
  resolution?: { width: number; height: number };
  trim?: { startSeconds?: number; endSeconds?: number };
  watermark?: WatermarkOptions;
  audioBitrateKbps?: number;
}

export interface MediaProcessingJobParams {
  inputPath: string;
  outputDirectory: string;
  options: MediaProcessingOptions;
}
