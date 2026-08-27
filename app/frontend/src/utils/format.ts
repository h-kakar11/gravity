import type { SpeedUnit } from "../types/settings";

// Shared display formatting for Progress fields (docs/ipc-contract.md) -- used by
// QueuePage and, later, any other screen that shows live job progress.

export function formatBytesPerSecond(bytesPerSecond?: number): string | null {
  if (bytesPerSecond === undefined || !Number.isFinite(bytesPerSecond) || bytesPerSecond <= 0) return null;
  const mbps = bytesPerSecond / (1024 * 1024);
  if (mbps >= 1) return `${mbps.toFixed(1)} MB/s`;
  const kbps = bytesPerSecond / 1024;
  return `${kbps.toFixed(0)} KB/s`;
}

// Like formatBytesPerSecond, but respects the user's downloads.speedUnits choice
// (settings.downloads.speedUnits, issue #59) instead of always auto-scaling MB/KB.
// "B" units are decimal SI (1000-based, e.g. KB/MB/GB); "iB" units are binary
// (1024-based, e.g. KiB/MiB/GiB) -- the distinction the unit names themselves promise.
// "Mbps" is megabits/sec (network-speed convention), not megabytes.
export function formatSpeedWithUnit(bytesPerSecond: number | undefined, unit: SpeedUnit): string | null {
  if (bytesPerSecond === undefined || !Number.isFinite(bytesPerSecond) || bytesPerSecond <= 0) return null;
  switch (unit) {
    case "KBps":
      return `${(bytesPerSecond / 1000).toFixed(0)} KB/s`;
    case "KiBps":
      return `${(bytesPerSecond / 1024).toFixed(0)} KiB/s`;
    case "MBps":
      return `${(bytesPerSecond / 1_000_000).toFixed(2)} MB/s`;
    case "MiBps":
      return `${(bytesPerSecond / (1024 * 1024)).toFixed(2)} MiB/s`;
    case "GBps":
      return `${(bytesPerSecond / 1_000_000_000).toFixed(2)} GB/s`;
    case "GiBps":
      return `${(bytesPerSecond / (1024 * 1024 * 1024)).toFixed(2)} GiB/s`;
    case "Mbps":
      return `${((bytesPerSecond * 8) / 1_000_000).toFixed(2)} Mb/s`;
  }
}

export function formatEta(etaSeconds?: number): string | null {
  if (etaSeconds === undefined || !Number.isFinite(etaSeconds) || etaSeconds < 0) return null;
  const totalSeconds = Math.round(etaSeconds);
  const hours = Math.floor(totalSeconds / 3600);
  const minutes = Math.floor((totalSeconds % 3600) / 60);
  const seconds = totalSeconds % 60;
  if (hours > 0) return `${hours}:${String(minutes).padStart(2, "0")}:${String(seconds).padStart(2, "0")} left`;
  return `${minutes}:${String(seconds).padStart(2, "0")} left`;
}

export function formatBytes(bytes?: number): string | null {
  if (bytes === undefined || !Number.isFinite(bytes) || bytes < 0) return null;
  if (bytes < 1024) return `${bytes} B`;
  const units = ["KB", "MB", "GB", "TB"];
  let value = bytes / 1024;
  let unitIndex = 0;
  while (value >= 1024 && unitIndex < units.length - 1) {
    value /= 1024;
    unitIndex += 1;
  }
  return `${value.toFixed(1)} ${units[unitIndex]}`;
}

export function formatTimestamp(iso?: string): string {
  if (!iso) return "";
  const date = new Date(iso);
  if (Number.isNaN(date.getTime())) return "";
  return date.toLocaleString();
}
