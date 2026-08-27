import { useEffect, useState } from "react";
import GlassCard from "../components/GlassCard";
import WatchFoldersSection from "../components/WatchFoldersSection";
import * as coreClient from "../services/coreClient";
import type { CommandResult } from "../types/ipc";
import type { Settings, SpeedUnit } from "../types/settings";
import { asErrorInfo } from "../utils/errors";
import styles from "./SettingsPage.module.css";

type SaveState = "idle" | "saving" | "saved" | "error";
type MediaEngineCapabilities = CommandResult["getMediaEngineCapabilities"];

const HW_ENCODER_LABELS: Record<keyof MediaEngineCapabilities["hardwareEncodersAvailable"], string> = {
  nvenc: "NVENC (NVIDIA)",
  amf: "AMF (AMD)",
  qsv: "Quick Sync (Intel)",
};

// Surfaces exactly which specific encoder will actually be used, not just an on/off toggle
// -- the probe itself (FFmpegEngine::AvailableEncoders(), cached once at construction) was
// already built in Phase 2.6; this is only the UI layer on top of it.
function describeHardwareEncoders(capabilities: MediaEngineCapabilities | null): string | undefined {
  if (!capabilities) return undefined;
  const detected = (Object.keys(capabilities.hardwareEncodersAvailable) as Array<
    keyof MediaEngineCapabilities["hardwareEncodersAvailable"]
  >).filter((key) => capabilities.hardwareEncodersAvailable[key]);

  if (detected.length === 0) {
    return "No hardware encoder detected on this ffmpeg -- falls back to CPU encoding (libopenh264/libx264).";
  }
  return `Detected: ${detected.map((key) => HW_ENCODER_LABELS[key]).join(", ")}`;
}

// Purely cosmetic: tauri-plugin-global-shortcut's accelerator syntax
// ("CommandOrControl+Shift+D") is what's actually stored and registered, and the input
// below keeps editing that raw form -- this only renders a friendlier preview next to it,
// e.g. "Ctrl+Shift+D". Gravity is Windows-only in practice, so CommandOrControl always
// resolves to Ctrl there; no per-platform branching needed. See issue #59.
function formatHotkeyForDisplay(accelerator: string): string {
  if (!accelerator.trim()) return "Disabled";
  return accelerator
    .split("+")
    .map((part) => (part === "CommandOrControl" || part === "CmdOrCtrl" ? "Ctrl" : part))
    .join("+");
}

function Field({
  label,
  hint,
  children,
}: {
  label: string;
  hint?: string;
  children: React.ReactNode;
}) {
  return (
    <div className={styles.field}>
      <div>
        <div className={styles.fieldLabel}>{label}</div>
        {hint && <div className={styles.fieldHint}>{hint}</div>}
      </div>
      {children}
    </div>
  );
}

export default function SettingsPage() {
  const [settings, setSettings] = useState<Settings | null>(null);
  const [loadError, setLoadError] = useState<string | null>(null);
  const [saveState, setSaveState] = useState<SaveState>("idle");
  const [saveError, setSaveError] = useState<string | null>(null);
  const [capabilities, setCapabilities] = useState<MediaEngineCapabilities | null>(null);

  useEffect(() => {
    coreClient
      .getSettings()
      .then(({ settings: loaded }) => setSettings(loaded))
      .catch((err) => setLoadError(asErrorInfo(err).message));
    // Best-effort -- the encoder probe is a "nice to know" surfaced from 2.6's cached
    // FFmpegEngine::AvailableEncoders(), never allowed to block the Settings page loading.
    coreClient
      .getMediaEngineCapabilities()
      .then(setCapabilities)
      .catch(() => {});
  }, []);

  if (loadError) {
    return (
      <div className={styles.wrap}>
        <div className={styles.errorText}>{loadError}</div>
      </div>
    );
  }
  if (!settings) {
    return (
      <div className={styles.wrap}>
        <div className={styles.statusText}>Loading settings...</div>
      </div>
    );
  }

  const update = <K extends keyof Settings>(section: K, patch: Partial<Settings[K]>) => {
    setSettings((prev) => (prev ? { ...prev, [section]: { ...prev[section], ...patch } } : prev));
    setSaveState("idle");
  };

  const save = async () => {
    setSaveState("saving");
    setSaveError(null);
    try {
      const { settings: saved } = await coreClient.updateSettings(settings);
      setSettings(saved);
      setSaveState("saved");
      // Best-effort: a saved hotkey binding should take effect immediately rather than
      // waiting for the next launch. Not fatal if it fails -- the new binding still lands
      // on the next app start via refresh_hotkeys() in run()'s setup().
      coreClient.refreshHotkeys().catch(() => {});
    } catch (err) {
      setSaveError(asErrorInfo(err).message);
      setSaveState("error");
    }
  };

  return (
    <div className={styles.wrap}>
      <GlassCard className={styles.section}>
        <h2 className={styles.sectionTitle}>General</h2>
        <Field label="Default output directory">
          <input
            className={styles.textInput}
            type="text"
            value={settings.general.defaultOutputDirectory}
            onChange={(e) => update("general", { defaultOutputDirectory: e.target.value })}
          />
        </Field>
        <Field label="Launch on startup">
          <input
            className={styles.checkbox}
            type="checkbox"
            checked={settings.general.launchOnStartup}
            onChange={(e) => update("general", { launchOnStartup: e.target.checked })}
          />
        </Field>
        <Field label="Show notifications">
          <input
            className={styles.checkbox}
            type="checkbox"
            checked={settings.general.showNotifications}
            onChange={(e) => update("general", { showNotifications: e.target.checked })}
          />
        </Field>
        <Field label="Keep running in the tray when closed" hint="Needed for watch folders and scheduled tasks">
          <input
            className={styles.checkbox}
            type="checkbox"
            checked={settings.general.minimizeToTrayOnClose}
            onChange={(e) => update("general", { minimizeToTrayOnClose: e.target.checked })}
          />
        </Field>
        <Field
          label="Paste link and download"
          hint={`Global hotkey, e.g. CommandOrControl+Shift+D. Leave empty to disable. Currently: ${formatHotkeyForDisplay(settings.general.hotkeyPasteAndDownload)}`}
        >
          <input
            className={styles.textInput}
            type="text"
            value={settings.general.hotkeyPasteAndDownload}
            onChange={(e) => update("general", { hotkeyPasteAndDownload: e.target.value })}
          />
        </Field>
        <Field
          label="Focus queue"
          hint={`Global hotkey, e.g. CommandOrControl+Shift+Q. Leave empty to disable. Currently: ${formatHotkeyForDisplay(settings.general.hotkeyFocusQueue)}`}
        >
          <input
            className={styles.textInput}
            type="text"
            value={settings.general.hotkeyFocusQueue}
            onChange={(e) => update("general", { hotkeyFocusQueue: e.target.value })}
          />
        </Field>
      </GlassCard>

      <GlassCard className={styles.section}>
        <h2 className={styles.sectionTitle}>Downloads</h2>
        {/* No separate "download directory" field here on purpose: General's "Default
            output directory" above is the one both the Download and Convert pages actually
            read (issue #54). A second directory field here was never wired to anything and
            just meant two settings claiming to control the same thing -- part of #18/#59's
            "which of these actually does something" confusion. */}
        <Field
          label="Filename template"
          hint="Not yet applied to downloads (tracked separately) -- saved here for when it is."
        >
          <input
            className={styles.textInput}
            type="text"
            value={settings.downloads.filenameTemplate}
            onChange={(e) => update("downloads", { filenameTemplate: e.target.value })}
          />
        </Field>
        <Field label="Concurrent downloads" hint="1-10">
          <input
            className={styles.numberInput}
            type="number"
            min={1}
            max={10}
            value={settings.downloads.concurrentDownloads}
            onChange={(e) => update("downloads", { concurrentDownloads: Number(e.target.value) })}
          />
        </Field>
        <Field label="Speed units">
          <select
            className={styles.selectInput}
            value={settings.downloads.speedUnits}
            onChange={(e) => update("downloads", { speedUnits: e.target.value as SpeedUnit })}
          >
            <option value="KBps">Kilobytes/sec (KB/s)</option>
            <option value="KiBps">Kibibytes/sec (KiB/s)</option>
            <option value="MBps">Megabytes/sec (MB/s)</option>
            <option value="MiBps">Mebibytes/sec (MiB/s)</option>
            <option value="GBps">Gigabytes/sec (GB/s)</option>
            <option value="GiBps">Gibibytes/sec (GiB/s)</option>
            <option value="Mbps">Megabits/sec (Mb/s)</option>
          </select>
        </Field>
      </GlassCard>

      <GlassCard className={styles.section}>
        <h2 className={styles.sectionTitle}>Processing</h2>
        <Field label="Hardware acceleration" hint={describeHardwareEncoders(capabilities)}>
          <input
            className={styles.checkbox}
            type="checkbox"
            checked={settings.processing.hardwareAccelerationEnabled}
            onChange={(e) => update("processing", { hardwareAccelerationEnabled: e.target.checked })}
          />
        </Field>
        <Field label="Default compression quality">
          <select
            className={styles.selectInput}
            value={settings.processing.defaultCompressionQuality}
            onChange={(e) =>
              update("processing", { defaultCompressionQuality: e.target.value as "low" | "medium" | "high" })
            }
          >
            <option value="low">Low</option>
            <option value="medium">Medium</option>
            <option value="high">High</option>
          </select>
        </Field>
        <Field
          label="Concurrent jobs"
          hint="1-25. Hardware encoders often cap how many sessions actually run at once, regardless of this setting."
        >
          <input
            className={styles.numberInput}
            type="number"
            min={1}
            max={25}
            value={settings.processing.concurrentJobs}
            onChange={(e) => update("processing", { concurrentJobs: Number(e.target.value) })}
          />
        </Field>
      </GlassCard>

      <GlassCard className={styles.section}>
        <h2 className={styles.sectionTitle}>Watch Folders</h2>
        <WatchFoldersSection />
      </GlassCard>

      <GlassCard className={styles.section}>
        <h2 className={styles.sectionTitle}>Advanced</h2>
        <Field label="Crash reporting">
          <input
            className={styles.checkbox}
            type="checkbox"
            checked={settings.privacy.crashReportingEnabled}
            onChange={(e) => update("privacy", { crashReportingEnabled: e.target.checked })}
          />
        </Field>
        <Field label="ffmpeg path" hint="Leave empty to auto-discover">
          <input
            className={styles.textInput}
            type="text"
            value={settings.advanced.ffmpegPath}
            onChange={(e) => update("advanced", { ffmpegPath: e.target.value })}
          />
        </Field>
        <Field label="Log level">
          <select
            className={styles.selectInput}
            value={settings.advanced.logLevel}
            onChange={(e) =>
              update("advanced", { logLevel: e.target.value as "DEBUG" | "INFO" | "WARNING" | "ERROR" })
            }
          >
            <option value="DEBUG">Debug</option>
            <option value="INFO">Info</option>
            <option value="WARNING">Warning</option>
            <option value="ERROR">Error</option>
          </select>
        </Field>
        <Field label="Allow network (UNC) paths" hint="Off by default for safety">
          <input
            className={styles.checkbox}
            type="checkbox"
            checked={settings.advanced.allowNetworkPaths}
            onChange={(e) => update("advanced", { allowNetworkPaths: e.target.checked })}
          />
        </Field>
      </GlassCard>

      <div className={styles.footer}>
        <button className={styles.saveButton} onClick={() => void save()} disabled={saveState === "saving"}>
          {saveState === "saving" ? "Saving..." : "Save"}
        </button>
        {saveState === "saved" && <span className={styles.statusText}>Saved</span>}
        {saveState === "error" && saveError && <span className={styles.errorText}>{saveError}</span>}
      </div>
    </div>
  );
}
