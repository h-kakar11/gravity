// The ONLY module in this app allowed to touch @tauri-apps/api directly (invoke/listen).
// Every other module -- hooks, pages, components -- goes through the typed functions
// exported here so the wire format (docs/ipc-contract.md) has exactly one place it can leak.
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import type { CoreCommand, CommandParams, CommandResult, CoreEvent, DownloadJobParams } from "../types/ipc";
import type { ErrorInfo } from "../types/error";
import type { MediaProcessingJobParams } from "../types/conversion";

function isErrorInfo(value: unknown): value is ErrorInfo {
  if (typeof value !== "object" || value === null) return false;
  const v = value as Record<string, unknown>;
  return (
    typeof v.code === "string" &&
    typeof v.category === "string" &&
    typeof v.message === "string" &&
    typeof v.details === "string" &&
    typeof v.recoverable === "boolean"
  );
}

// The Rust `send_core_command` command rejects with a stringified error whose exact shape
// depends on how the parallel Rust workstream serializes its Result::Err. We defensively
// accept either a JSON-encoded ErrorInfo or a plain string message, and never let a
// malformed rejection reach a caller as anything other than a well-formed ErrorInfo.
function toErrorInfo(reason: unknown): ErrorInfo {
  if (isErrorInfo(reason)) return reason;

  if (typeof reason === "string") {
    try {
      const parsed: unknown = JSON.parse(reason);
      if (isErrorInfo(parsed)) return parsed;
      // Parsed cleanly but isn't an ErrorInfo shape (e.g. Rust sent back a bare object).
      return {
        code: "E_UNKNOWN",
        category: "UNKNOWN",
        message: "The core process reported an error.",
        details: reason,
        recoverable: false,
      };
    } catch {
      // Not JSON at all -- a plain string rejection message.
      return {
        code: "E_UNKNOWN",
        category: "UNKNOWN",
        message: reason,
        details: reason,
        recoverable: false,
      };
    }
  }

  return {
    code: "E_UNKNOWN",
    category: "UNKNOWN",
    message: "An unexpected error occurred talking to the core process.",
    details: reason instanceof Error ? (reason.stack ?? reason.message) : String(reason),
    recoverable: false,
  };
}

// Built on the Rust `send_core_command` Tauri command, which forwards to mediatool-core over
// stdio NDJSON (see docs/ipc-contract.md) and resolves with the already-unwrapped result
// object (i.e. CommandResult[C]), or rejects with a stringified error on failure.
export async function sendCommand<C extends CoreCommand>(
  command: C,
  params: CommandParams[C],
): Promise<CommandResult[C]> {
  try {
    return await invoke<CommandResult[C]>("send_core_command", { command, params });
  } catch (reason) {
    throw toErrorInfo(reason);
  }
}

// Subscribes to the "core-event" Tauri event (Rust's forwarding of unsolicited core NDJSON
// events, e.g. jobProgress). Returns an unsubscribe function; safe to call even before the
// underlying `listen()` promise has resolved.
export function subscribeToJobEvents(callback: (event: CoreEvent) => void): () => void {
  let unlisten: (() => void) | null = null;
  let cancelled = false;

  listen<CoreEvent>("core-event", (event) => {
    callback(event.payload);
  }).then((fn) => {
    if (cancelled) {
      fn();
    } else {
      unlisten = fn;
    }
  });

  return () => {
    cancelled = true;
    if (unlisten) unlisten();
  };
}

// Thin, typed conveniences over sendCommand -- kept in this module so nothing else needs to
// know the command name strings from docs/ipc-contract.md.
export const createJob = (params: CommandParams["createJob"]) => sendCommand("createJob", params);
export const getJob = (jobId: string) => sendCommand("getJob", { jobId });
export const listJobs = () => sendCommand("listJobs", {});
export const listJobHistory = (limit?: number) => sendCommand("listJobHistory", { limit });
export const cancelJob = (jobId: string) => sendCommand("cancelJob", { jobId });
export const pauseJob = (jobId: string) => sendCommand("pauseJob", { jobId });
export const resumeJob = (jobId: string) => sendCommand("resumeJob", { jobId });
export const retryJob = (jobId: string) => sendCommand("retryJob", { jobId });
export const inspectFile = (path: string) => sendCommand("inspectFile", { path });
export const inspectDownloadUrl = (url: string) => sendCommand("inspectDownloadUrl", { url });
export const createDownloadJob = (params: DownloadJobParams) =>
  sendCommand("createJob", { type: "DOWNLOAD", params: params as unknown as Record<string, unknown> });
export const createConversionJob = (params: MediaProcessingJobParams) =>
  sendCommand("createJob", { type: "CONVERSION", params: params as unknown as Record<string, unknown> });
export const createCompressionJob = (params: MediaProcessingJobParams) =>
  sendCommand("createJob", { type: "COMPRESSION", params: params as unknown as Record<string, unknown> });
export const getCapabilities = (path: string) => sendCommand("getCapabilities", { path });
export const getSettings = () => sendCommand("getSettings", {});
export const updateSettings = (settings: CommandParams["updateSettings"]["settings"]) =>
  sendCommand("updateSettings", { settings });
export const getHardwareInfo = () => sendCommand("getHardwareInfo", {});

// Reveals a completed download's output file in Windows Explorer via the Rust
// `open_containing_folder` Tauri command (app/desktop/src-tauri/src/lib.rs) -- never a raw
// shell command from React (spec section 37).
export async function openContainingFolder(filePath: string): Promise<void> {
  try {
    await invoke("open_containing_folder", { path: filePath });
  } catch (reason) {
    throw toErrorInfo(reason);
  }
}

// Re-registers the global hotkeys (Phase 4.4) from whatever Settings currently holds --
// call after a successful updateSettings so a changed or cleared binding takes effect
// immediately instead of waiting for the next launch.
export async function refreshHotkeys(): Promise<void> {
  try {
    await invoke("refresh_hotkeys");
  } catch (reason) {
    throw toErrorInfo(reason);
  }
}

// Subscribes to the two raw Tauri events hotkeys.rs emits directly (not "core-event" --
// these never touch the C++ core). Returns an unsubscribe function, safe to call even
// before the underlying listen() promises resolve.
export function subscribeToHotkeyEvents(handlers: {
  onPasteAndDownload?: (url: string) => void;
  onFocusQueue?: () => void;
}): () => void {
  const unlistenFns: Array<() => void> = [];
  let cancelled = false;

  const track = (promise: Promise<() => void>) => {
    promise.then((fn) => {
      if (cancelled) fn();
      else unlistenFns.push(fn);
    });
  };

  if (handlers.onPasteAndDownload) {
    track(
      listen<{ url: string }>("hotkey-paste-and-download", (event) => {
        handlers.onPasteAndDownload?.(event.payload.url);
      }),
    );
  }
  if (handlers.onFocusQueue) {
    track(
      listen("hotkey-focus-queue", () => {
        handlers.onFocusQueue?.();
      }),
    );
  }

  return () => {
    cancelled = true;
    unlistenFns.forEach((fn) => fn());
  };
}
