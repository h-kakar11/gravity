// Mirrors engines/ffmpeg/FFmpegArgBuilder.h's MediaProcessingOptions and
// core/jobs/MediaProcessingJob.h's Options -- the params shape for a CONVERSION or
// COMPRESSION createJob call (docs/ipc-contract.md).

export type MediaQuality = "low" | "medium" | "high" | "lossless";
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
  // "lossless" is a Pro-tier value -- never offer it as selectable in the UI (idealist.md:
  // build the affordance visibly-present-but-inert, not wired to anything real). The
  // server rejects it unconditionally regardless of what the UI sends.
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
