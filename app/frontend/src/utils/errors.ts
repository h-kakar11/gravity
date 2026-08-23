import type { ErrorInfo } from "../types/error";

// Shared by every page that surfaces a coreClient rejection directly (DevConsole,
// DownloaderPage) -- coreClient already normalizes rejections to ErrorInfo, but a page
// may also catch a plain thrown value from its own validation, so this stays defensive.
export function asErrorInfo(err: unknown): ErrorInfo {
  if (err && typeof err === "object" && "category" in err && "message" in err) {
    return err as ErrorInfo;
  }
  return {
    code: "E_UNKNOWN",
    category: "UNKNOWN",
    message: String(err),
    details: String(err),
    recoverable: false,
  };
}
