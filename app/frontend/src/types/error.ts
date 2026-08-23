// Mirrors core/errors/ErrorInfo.h. `message` is safe to show a user; `details` is
// developer diagnostics only (spec section 26).
export type ErrorCategory =
  | "FILE_NOT_FOUND"
  | "INVALID_FILE"
  | "UNSUPPORTED_FORMAT"
  | "ENGINE_FAILURE"
  | "DOWNLOAD_FAILURE"
  | "NETWORK_ERROR"
  | "PERMISSION_ERROR"
  | "DISK_SPACE_ERROR"
  | "CANCELLED"
  | "UNKNOWN";

export interface ErrorInfo {
  code: string;
  category: ErrorCategory;
  message: string;
  details: string;
  recoverable: boolean;
}
