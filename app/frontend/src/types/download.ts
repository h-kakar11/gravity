// Mirrors core/downloads/QualityPreset.h and core/downloads/IDownloadProvider.h exactly.
// Wire values are UPPER_SNAKE_CASE strings -- do not translate casing on this side of the
// boundary. See docs/ipc-contract.md's "QualityPreset"/"DownloadFormat"/"DownloadMetadata"
// sections.

export type QualityPreset = "BEST" | "2160P" | "1440P" | "1080P" | "720P" | "480P" | "AUDIO_ONLY";

export const QUALITY_PRESET_LABELS: Record<QualityPreset, string> = {
  BEST: "Best available",
  "2160P": "2160p (4K)",
  "1440P": "1440p (2K)",
  "1080P": "1080p",
  "720P": "720p",
  "480P": "480p",
  AUDIO_ONLY: "Audio only",
};

export interface DownloadFormat {
  formatId: string;
  extension?: string;
  resolution?: string;
  width?: number;
  height?: number;
  fps?: number;
  videoCodec?: string;
  audioCodec?: string;
  videoBitrateKbps?: number;
  audioBitrateKbps?: number;
  filesizeBytes?: number;
  approxFilesizeBytes?: number;
  hasVideo: boolean;
  hasAudio: boolean;
}

export interface DownloadMetadata {
  title: string;
  uploader?: string;
  durationSeconds?: number;
  webpageUrl?: string;
  thumbnailUrl?: string;
  extractor?: string;
  playlistIndex?: number;
  playlistCount?: number;
  formats: DownloadFormat[];
}

// One downloadable video inside a playlist, resolved shallowly -- no formats/thumbnail
// here, because enumerating a playlist deliberately skips the per-video extractor round
// trip. Each entry's own DownloadJob fetches its full metadata when it runs.
export interface PlaylistEntry {
  // 1-based position among downloadable entries (unavailable ones are already dropped), and
  // the number used for the "01 - " filename prefix.
  index: number;
  url: string;
  title: string;
  durationSeconds?: number;
}

export interface PlaylistInfo {
  title: string;
  uploader?: string;
  webpageUrl?: string;
  count: number;
  // The playlist had more entries than the backend's enumeration cap; the tail was dropped.
  truncated: boolean;
  entries: PlaylistEntry[];
}
