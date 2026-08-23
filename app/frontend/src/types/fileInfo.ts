// Mirrors core/filesystem/FileInfo.h.
export type FileCategory =
  | "VIDEO"
  | "AUDIO"
  | "IMAGE"
  | "DOCUMENT"
  | "TEXT"
  | "ARCHIVE"
  | "UNKNOWN";

export interface FileInfo {
  path: string;
  filename: string;
  extension: string;
  category: FileCategory;
  sizeBytes: number;
  mimeType?: string;
  durationSeconds?: number;
  width?: number;
  height?: number;
  videoCodec?: string;
  audioCodec?: string;
  bitrate?: number;
  fps?: number;
}
