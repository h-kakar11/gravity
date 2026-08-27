import { describe, expect, it } from "vitest";
import { formatSpeedWithUnit } from "./format";

describe("formatSpeedWithUnit", () => {
  it("returns null for undefined/zero/negative/non-finite input", () => {
    expect(formatSpeedWithUnit(undefined, "MBps")).toBeNull();
    expect(formatSpeedWithUnit(0, "MBps")).toBeNull();
    expect(formatSpeedWithUnit(-5, "MBps")).toBeNull();
    expect(formatSpeedWithUnit(NaN, "MBps")).toBeNull();
  });

  it("formats decimal (SI, 1000-based) byte units", () => {
    expect(formatSpeedWithUnit(1_000, "KBps")).toBe("1 KB/s");
    expect(formatSpeedWithUnit(1_000_000, "MBps")).toBe("1.00 MB/s");
    expect(formatSpeedWithUnit(1_000_000_000, "GBps")).toBe("1.00 GB/s");
  });

  it("formats binary (1024-based) byte units distinctly from their decimal counterparts", () => {
    expect(formatSpeedWithUnit(1024, "KiBps")).toBe("1 KiB/s");
    expect(formatSpeedWithUnit(1024 * 1024, "MiBps")).toBe("1.00 MiB/s");
    expect(formatSpeedWithUnit(1024 * 1024 * 1024, "GiBps")).toBe("1.00 GiB/s");
    // Same raw byte rate, different unit choice -> different displayed number, since
    // decimal and binary prefixes disagree (the whole point of offering both).
    expect(formatSpeedWithUnit(1_000_000, "MiBps")).not.toBe(formatSpeedWithUnit(1_000_000, "MBps"));
  });

  it("formats Mbps as megabits (x8), not megabytes", () => {
    expect(formatSpeedWithUnit(1_000_000, "Mbps")).toBe("8.00 Mb/s");
  });
});
