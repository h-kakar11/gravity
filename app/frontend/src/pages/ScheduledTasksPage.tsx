import { useCallback, useEffect, useState } from "react";
import GlassCard from "../components/GlassCard";
import * as coreClient from "../services/coreClient";
import type { ScheduledTaskConfig, ScheduledTaskJobType } from "../types/automation";
import { QUALITY_PRESET_LABELS, type QualityPreset } from "../types/download";
import type { Preset } from "../types/preset";
import styles from "./ScheduledTasksPage.module.css";

// Scheduled Tasks (Phase 4.3): a cron schedule that fires a createJob call in the
// background (app/desktop/src-tauri/src/scheduler.rs owns the actual polling/firing).
// Cron uses the `cron` crate's 6-field syntax (seconds first) -- the quick-pick list below
// exists so most users never have to type one by hand.
const CRON_PRESETS: Array<{ label: string; expression: string }> = [
  { label: "Every hour", expression: "0 0 * * * *" },
  { label: "Every day at 2:00 AM", expression: "0 0 2 * * *" },
  { label: "Every day at 9:00 PM", expression: "0 0 21 * * *" },
  { label: "Every Sunday at 3:00 AM", expression: "0 0 3 * * 0" },
];

const QUALITY_OPTIONS: QualityPreset[] = ["BEST", "2160P", "1440P", "1080P", "720P", "480P", "AUDIO_ONLY"];

function jobTypeLabel(type: ScheduledTaskJobType): string {
  return type === "CONVERSION" ? "Convert" : type === "COMPRESSION" ? "Compress" : "Download";
}

export default function ScheduledTasksPage() {
  const [tasks, setTasks] = useState<ScheduledTaskConfig[]>([]);
  const [presets, setPresets] = useState<Preset[]>([]);
  const [loadError, setLoadError] = useState<string | null>(null);

  const [name, setName] = useState("");
  const [jobType, setJobType] = useState<ScheduledTaskJobType>("CONVERSION");
  const [cronExpression, setCronExpression] = useState(CRON_PRESETS[1].expression);
  const [inputPath, setInputPath] = useState("");
  const [outputDirectory, setOutputDirectory] = useState("");
  const [presetId, setPresetId] = useState("");
  const [url, setUrl] = useState("");
  const [quality, setQuality] = useState<QualityPreset>("BEST");
  const [creating, setCreating] = useState(false);
  const [createError, setCreateError] = useState<string | null>(null);

  const refreshTasks = useCallback(() => {
    coreClient
      .listScheduledTasks()
      .then(setTasks)
      .catch((err) => setLoadError(err instanceof Error ? err.message : String(err)));
  }, []);

  useEffect(() => {
    refreshTasks();
    coreClient
      .listPresets()
      .then(({ presets: all }) => setPresets(all))
      .catch(() => {});
  }, [refreshTasks]);

  const isMediaJob = jobType === "CONVERSION" || jobType === "COMPRESSION";
  const presetsForType = presets.filter((preset) => preset.kind === jobType);
  const canCreate = name.trim().length > 0 && cronExpression.trim().length > 0 && outputDirectory.trim().length > 0 &&
    (isMediaJob ? inputPath.trim().length > 0 && presetId.length > 0 : url.trim().length > 0);

  const handleCreate = async () => {
    setCreating(true);
    setCreateError(null);
    try {
      const params = isMediaJob
        ? {
            inputPath: inputPath.trim(),
            outputDirectory: outputDirectory.trim(),
            options: presetsForType.find((p) => p.id === presetId)?.options ?? {},
          }
        : { url: url.trim(), outputDirectory: outputDirectory.trim(), quality };

      await coreClient.addScheduledTask({ name: name.trim(), cronExpression: cronExpression.trim(), jobType, params });
      setName("");
      setInputPath("");
      setUrl("");
      setPresetId("");
      refreshTasks();
    } catch (err) {
      setCreateError(err instanceof Error ? err.message : String(err));
    } finally {
      setCreating(false);
    }
  };

  const handleToggle = async (task: ScheduledTaskConfig) => {
    try {
      await coreClient.updateScheduledTask({ id: task.id, enabled: !task.enabled });
      refreshTasks();
    } catch {
      // Best-effort.
    }
  };

  const handleDelete = async (id: string) => {
    try {
      await coreClient.removeScheduledTask(id);
      refreshTasks();
    } catch {
      // Best-effort.
    }
  };

  return (
    <div className={styles.wrap}>
      <h1 className={styles.title}>Scheduled Tasks</h1>

      <GlassCard className={styles.section}>
        {loadError && <div className={styles.error}>{loadError}</div>}
        {tasks.length === 0 ? (
          <div className={styles.empty}>No scheduled tasks yet.</div>
        ) : (
          <div className={styles.list}>
            {tasks.map((task) => (
              <div key={task.id} className={styles.row}>
                <div className={styles.rowMain}>
                  <div className={styles.rowTitle}>
                    <span className={styles.typeBadge}>{jobTypeLabel(task.jobType)}</span>
                    {task.name}
                  </div>
                  <div className={styles.rowMeta}>{task.cronExpression}</div>
                </div>
                <div className={styles.rowActions}>
                  <label className={styles.enabledLabel}>
                    <input type="checkbox" checked={task.enabled} onChange={() => void handleToggle(task)} />
                    Enabled
                  </label>
                  <button type="button" className={styles.deleteButton} onClick={() => void handleDelete(task.id)}>
                    Delete
                  </button>
                </div>
              </div>
            ))}
          </div>
        )}
      </GlassCard>

      <GlassCard className={styles.section}>
        <h2 className={styles.sectionTitle}>New scheduled task</h2>

        <div className={styles.field}>
          <label className={styles.fieldLabel}>Name</label>
          <input className={styles.textInput} type="text" value={name} onChange={(e) => setName(e.target.value)} />
        </div>

        <div className={styles.field}>
          <label className={styles.fieldLabel}>Type</label>
          <select
            className={styles.selectInput}
            value={jobType}
            onChange={(e) => {
              setJobType(e.target.value as ScheduledTaskJobType);
              setPresetId("");
            }}
          >
            <option value="CONVERSION">Convert</option>
            <option value="COMPRESSION">Compress</option>
            <option value="DOWNLOAD">Download</option>
          </select>
        </div>

        {isMediaJob ? (
          <>
            <div className={styles.field}>
              <label className={styles.fieldLabel}>Input file</label>
              <input
                className={styles.textInput}
                type="text"
                placeholder="D:\Videos\clip.mov"
                value={inputPath}
                onChange={(e) => setInputPath(e.target.value)}
              />
            </div>
            <div className={styles.field}>
              <label className={styles.fieldLabel}>Preset</label>
              <select className={styles.selectInput} value={presetId} onChange={(e) => setPresetId(e.target.value)}>
                <option value="">
                  {presetsForType.length === 0 ? "No presets saved for this type" : "Choose a preset..."}
                </option>
                {presetsForType.map((preset) => (
                  <option key={preset.id} value={preset.id}>
                    {preset.name}
                  </option>
                ))}
              </select>
            </div>
          </>
        ) : (
          <>
            <div className={styles.field}>
              <label className={styles.fieldLabel}>URL</label>
              <input
                className={styles.textInput}
                type="text"
                placeholder="https://..."
                value={url}
                onChange={(e) => setUrl(e.target.value)}
              />
            </div>
            <div className={styles.field}>
              <label className={styles.fieldLabel}>Quality</label>
              <select className={styles.selectInput} value={quality} onChange={(e) => setQuality(e.target.value as QualityPreset)}>
                {QUALITY_OPTIONS.map((preset) => (
                  <option key={preset} value={preset}>
                    {QUALITY_PRESET_LABELS[preset]}
                  </option>
                ))}
              </select>
            </div>
          </>
        )}

        <div className={styles.field}>
          <label className={styles.fieldLabel}>Output folder</label>
          <input
            className={styles.textInput}
            type="text"
            placeholder="D:\Converted"
            value={outputDirectory}
            onChange={(e) => setOutputDirectory(e.target.value)}
          />
        </div>

        <div className={styles.field}>
          <label className={styles.fieldLabel}>Schedule</label>
          <div className={styles.scheduleRow}>
            <select
              className={styles.selectInput}
              value=""
              onChange={(e) => {
                if (e.target.value) setCronExpression(e.target.value);
              }}
            >
              <option value="">Quick pick...</option>
              {CRON_PRESETS.map((preset) => (
                <option key={preset.expression} value={preset.expression}>
                  {preset.label}
                </option>
              ))}
            </select>
            <input
              className={styles.textInput}
              type="text"
              value={cronExpression}
              onChange={(e) => setCronExpression(e.target.value)}
            />
          </div>
          <div className={styles.fieldHint}>6-field cron (seconds first), e.g. "0 0 2 * * *" for daily at 2 AM.</div>
        </div>

        <div className={styles.footer}>
          <button type="button" className={styles.submitButton} onClick={() => void handleCreate()} disabled={!canCreate || creating}>
            {creating ? "Creating..." : "Create task"}
          </button>
        </div>
        {createError && <div className={styles.error}>{createError}</div>}
      </GlassCard>
    </div>
  );
}
