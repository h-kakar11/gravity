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
//
// Two axes, and the unit names promise both:
//   byte units (capital B: KB/MB/GB) vs bit units (lowercase b: Kb/Mb/Gb) -- 8 bits to
//   the byte, and bit units are the network-speed convention;
//   decimal SI (1000-based: KB, Mb) vs binary (1024-based: KiB, Mib).
//
// Every member of SpeedUnit must appear here. The `never` assertion at the bottom is what
// enforces that at compile time: the five bit-rate units added to SpeedUnit (Kbps, Kibps,
// Mibps, Gbps, Gibps) and offered in the Settings dropdown had no case here at all, so the
// switch fell off the end and returned `undefined` -- every call site coalesces that to
// "?", which is why picking one of them made the speed readout stop showing a speed.
export function formatSpeedWithUnit(bytesPerSecond: number | undefined, unit: SpeedUnit): string | null {
  if (bytesPerSecond === undefined || !Number.isFinite(bytesPerSecond) || bytesPerSecond <= 0) return null;
  const bitsPerSecond = bytesPerSecond * 8;
  switch (unit) {
    // --- bytes, decimal ---
    case "KBps":
      return `${(bytesPerSecond / 1000).toFixed(0)} KB/s`;
    case "MBps":
      return `${(bytesPerSecond / 1_000_000).toFixed(2)} MB/s`;
    case "GBps":
      return `${(bytesPerSecond / 1_000_000_000).toFixed(2)} GB/s`;
    // --- bytes, binary ---
    case "KiBps":
      return `${(bytesPerSecond / 1024).toFixed(0)} KiB/s`;
    case "MiBps":
      return `${(bytesPerSecond / (1024 * 1024)).toFixed(2)} MiB/s`;
    case "GiBps":
      return `${(bytesPerSecond / (1024 * 1024 * 1024)).toFixed(2)} GiB/s`;
    // --- bits, decimal ---
    case "Kbps":
      return `${(bitsPerSecond / 1000).toFixed(0)} Kb/s`;
    case "Mbps":
      return `${(bitsPerSecond / 1_000_000).toFixed(2)} Mb/s`;
    case "Gbps":
      return `${(bitsPerSecond / 1_000_000_000).toFixed(2)} Gb/s`;
    // --- bits, binary ---
    case "Kibps":
      return `${(bitsPerSecond / 1024).toFixed(0)} Kib/s`;
    case "Mibps":
      return `${(bitsPerSecond / (1024 * 1024)).toFixed(2)} Mib/s`;
    case "Gibps":
      return `${(bitsPerSecond / (1024 * 1024 * 1024)).toFixed(2)} Gib/s`;
    default: {
      // Adding a member to SpeedUnit without a case above is a compile error here, not a
      // silent `undefined` at runtime.
      const exhaustive: never = unit;
      void exhaustive;
      return null;
    }
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
