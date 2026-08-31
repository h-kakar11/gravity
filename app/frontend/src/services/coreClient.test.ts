import { describe, expect, it, vi } from "vitest";
import { invoke } from "@tauri-apps/api/core";
import { sendCommand } from "./coreClient";

// coreClient.ts is the only module allowed to touch @tauri-apps/api directly -- mocking
// it here (rather than mocking coreClient itself, as other tests do) is what lets this
// file actually exercise toErrorInfo's branches, which are only reachable through
// sendCommand's catch block and are not themselves exported.
vi.mock("@tauri-apps/api/core", () => ({
  invoke: vi.fn(),
}));

vi.mock("@tauri-apps/api/event", () => ({
  listen: vi.fn(),
}));

describe("sendCommand error normalization (toErrorInfo)", () => {
  it("passes through a rejection that is already ErrorInfo-shaped", async () => {
    const errorInfo = {
      code: "E_JOB_NOT_FOUND",
      category: "UNKNOWN",
      message: "No such job.",
      details: "jobId=abc",
      recoverable: false,
    };
    vi.mocked(invoke).mockRejectedValue(errorInfo);

    await expect(sendCommand("listJobs", {})).rejects.toEqual(errorInfo);
  });

  it("parses a JSON-encoded ErrorInfo string rejection", async () => {
    const errorInfo = {
      code: "E_NETWORK",
      category: "NETWORK_ERROR",
      message: "Connection lost.",
      details: "",
      recoverable: true,
    };
    vi.mocked(invoke).mockRejectedValue(JSON.stringify(errorInfo));

    await expect(sendCommand("listJobs", {})).rejects.toEqual(errorInfo);
  });

  it("falls back to a generic E_UNKNOWN for a JSON string that isn't ErrorInfo-shaped", async () => {
    vi.mocked(invoke).mockRejectedValue(JSON.stringify({ some: "other shape" }));

    await expect(sendCommand("listJobs", {})).rejects.toMatchObject({
      code: "E_UNKNOWN",
      category: "UNKNOWN",
      details: JSON.stringify({ some: "other shape" }),
    });
  });

  it("falls back to a generic E_UNKNOWN for a plain non-JSON string rejection", async () => {
    vi.mocked(invoke).mockRejectedValue("mediatool-core is not responding");

    await expect(sendCommand("listJobs", {})).rejects.toMatchObject({
      code: "E_UNKNOWN",
      category: "UNKNOWN",
      message: "mediatool-core is not responding",
    });
  });

  it("falls back to a generic E_UNKNOWN for a non-string, non-ErrorInfo rejection", async () => {
    vi.mocked(invoke).mockRejectedValue(new Error("boom"));

    await expect(sendCommand("listJobs", {})).rejects.toMatchObject({
      code: "E_UNKNOWN",
      category: "UNKNOWN",
      recoverable: false,
    });
  });

  it("resolves normally when invoke succeeds", async () => {
    vi.mocked(invoke).mockResolvedValue({ jobs: [] });

    await expect(sendCommand("listJobs", {})).resolves.toEqual({ jobs: [] });
  });
});
