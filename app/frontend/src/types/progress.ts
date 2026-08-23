// Mirrors core/jobs/Progress.h. Every field except statusMessage is optional because not
// every operation has byte-oriented or time-oriented progress (spec section 7).
export interface Progress {
  percentage?: number; // 0-100
  processedBytes?: number;
  totalBytes?: number;
  speedBytesPerSecond?: number;
  etaSeconds?: number;
  currentItem?: string; // e.g. "file 43 of 100"
  statusMessage: string;
}
