// The only module allowed to touch @tauri-apps/plugin-notification directly, mirroring
// coreClient.ts's rule for @tauri-apps/api -- one thin wrapper per external integration
// surface (Phase 4.5).
import { isPermissionGranted, requestPermission, sendNotification } from "@tauri-apps/plugin-notification";
import * as coreClient from "./coreClient";

let permissionChecked = false;
let permissionGranted = false;

async function ensurePermission(): Promise<boolean> {
  if (permissionChecked) return permissionGranted;
  permissionGranted = await isPermissionGranted();
  if (!permissionGranted) {
    permissionGranted = (await requestPermission()) === "granted";
  }
  permissionChecked = true;
  return permissionGranted;
}

// Fires an OS toast, gated by Settings' `general.showNotifications` (checked fresh on every
// call rather than cached -- same "simplicity over premature optimization" tradeoff
// tray.rs's should_minimize_to_tray makes: this is a fast local IPC call, and a toggle the
// user just flipped in Settings should take effect on the very next notification, not
// whenever some cache happens to expire). Best-effort: a failure here should never break
// the job/event flow that triggered it.
export async function notify(title: string, body: string): Promise<void> {
  try {
    const { settings } = await coreClient.getSettings();
    if (!settings.general.showNotifications) return;
    if (!(await ensurePermission())) return;
    sendNotification({ title, body });
  } catch {
    // Notification delivery is never allowed to surface as a user-facing error.
  }
}
