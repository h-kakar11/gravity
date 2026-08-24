// Mirrors the getVersionInfo IPC command's result (app/core/main.cpp's HandleGetVersionInfo).
// ffmpegVersion/ytDlpVersion are best-effort and omitted, not empty strings, when that
// dependency isn't available -- the About panel must say so plainly rather than show a
// misleading blank.
export interface VersionInfo {
  gravityVersion: string;
  ffmpegVersion?: string;
  ytDlpVersion?: string;
}
