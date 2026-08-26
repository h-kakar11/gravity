import { useCallback, useEffect, useState } from "react";
import * as coreClient from "../services/coreClient";
import type { AutomationJobType, WatchFolderConfig } from "../types/automation";
import type { Preset } from "../types/preset";
import styles from "./WatchFoldersSection.module.css";

// Watch Folders (Phase 4.1) management UI. The Rust backend (watch_folders.rs) auto-submits
// a createJob call using whatever `defaultOptions` were configured when the folder was
// added -- since `outputFormat` is a required field on every CONVERSION/COMPRESSION job
// (docs/ipc-contract.md), a folder with no options at all would silently fail every single
// arrival. So this UI requires picking an existing Convert/Compress preset (4.6) rather than
// letting a folder be added with empty options -- a save-a-preset-first flow beats a
// watch folder that quietly never works.
export default function WatchFoldersSection() {
  const [folders, setFolders] = useState<WatchFolderConfig[]>([]);
  const [presets, setPresets] = useState<Preset[]>([]);
  const [path, setPath] = useState("");
  const [jobType, setJobType] = useState<AutomationJobType>("CONVERSION");
  const [presetId, setPresetId] = useState("");
  const [adding, setAdding] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const refreshFolders = useCallback(() => {
    coreClient
      .listWatchFolders()
      .then(setFolders)
      .catch(() => {
        // Best-effort -- this section just stays empty if Rust can't be reached.
      });
  }, []);

  useEffect(() => {
    refreshFolders();
    coreClient
      .listPresets()
      .then(({ presets: all }) => setPresets(all))
      .catch(() => {});
  }, [refreshFolders]);

  const presetsForType = presets.filter((preset) => preset.kind === jobType);

  const handleAdd = async () => {
    const trimmedPath = path.trim();
    const preset = presetsForType.find((p) => p.id === presetId);
    if (!trimmedPath || !preset) return;

    setAdding(true);
    setError(null);
    try {
      await coreClient.addWatchFolder(trimmedPath, jobType, preset.options);
      setPath("");
      setPresetId("");
      refreshFolders();
    } catch {
      setError("Could not add that folder -- check the path exists.");
    } finally {
      setAdding(false);
    }
  };

  const handleRemove = async (folderPath: string) => {
    try {
      await coreClient.removeWatchFolder(folderPath);
      refreshFolders();
    } catch {
      // Best-effort.
    }
  };

  return (
    <div className={styles.wrap}>
      {folders.length === 0 ? (
        <div className={styles.empty}>No watch folders configured.</div>
      ) : (
        <div className={styles.list}>
          {folders.map((folder) => (
            <div key={folder.path} className={styles.row}>
              <div className={styles.rowMain}>
                <div className={styles.rowPath}>{folder.path}</div>
                <div className={styles.rowMeta}>{folder.jobType === "CONVERSION" ? "Convert" : "Compress"}</div>
              </div>
              <button type="button" className={styles.removeButton} onClick={() => void handleRemove(folder.path)}>
                Remove
              </button>
            </div>
          ))}
        </div>
      )}

      <div className={styles.addRow}>
        <input
          className={styles.pathInput}
          type="text"
          placeholder="D:\WatchThisFolder"
          value={path}
          onChange={(e) => setPath(e.target.value)}
        />
        <select
          className={styles.typeSelect}
          value={jobType}
          onChange={(e) => {
            setJobType(e.target.value as AutomationJobType);
            setPresetId("");
          }}
        >
          <option value="CONVERSION">Convert</option>
          <option value="COMPRESSION">Compress</option>
        </select>
        <select className={styles.typeSelect} value={presetId} onChange={(e) => setPresetId(e.target.value)}>
          <option value="">
            {presetsForType.length === 0 ? "No presets saved for this type" : "Choose a preset..."}
          </option>
          {presetsForType.map((preset) => (
            <option key={preset.id} value={preset.id}>
              {preset.name}
            </option>
          ))}
        </select>
        <button type="button" className={styles.addButton} onClick={() => void handleAdd()} disabled={adding || !path.trim() || !presetId}>
          {adding ? "Adding..." : "Add"}
        </button>
      </div>
      {presetsForType.length === 0 && (
        <div className={styles.hint}>
          Save a {jobType === "CONVERSION" ? "Convert" : "Compress"} preset first (from that screen's "Save preset"
          control), then use it here.
        </div>
      )}
      {error && <div className={styles.error}>{error}</div>}
    </div>
  );
}
