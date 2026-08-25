import { useCallback, useEffect, useState } from "react";
import * as coreClient from "../services/coreClient";
import type { Preset, PresetKind } from "../types/preset";
import styles from "./PresetBar.module.css";

interface PresetBarProps {
  kind: PresetKind;
  // Read fresh at save time (not captured once) so "Save preset" always stores whatever
  // the form currently holds.
  currentOptions: () => Record<string, unknown>;
  onApply: (options: Record<string, unknown>) => void;
}

// Multi-Profile Presets (Phase 4.6): a picker + "save as preset" control shared by
// ConvertPage and DownloaderPage. Presets are opaque option bags scoped by `kind`
// (DOWNLOAD/CONVERSION/COMPRESSION) -- this component only ever round-trips them through
// listPresets/savePreset/deletePreset, never interprets the `options` shape itself.
export default function PresetBar({ kind, currentOptions, onApply }: PresetBarProps) {
  const [presets, setPresets] = useState<Preset[]>([]);
  const [selectedId, setSelectedId] = useState("");
  const [nameDraft, setNameDraft] = useState("");
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const refresh = useCallback(() => {
    coreClient
      .listPresets()
      .then(({ presets: all }) => setPresets(all.filter((p) => p.kind === kind)))
      .catch(() => {
        // Best-effort -- presets are a convenience layer, never allowed to block the page.
      });
  }, [kind]);

  useEffect(() => {
    refresh();
  }, [refresh]);

  const handleApply = (id: string) => {
    setSelectedId(id);
    const preset = presets.find((p) => p.id === id);
    if (preset) onApply(preset.options);
  };

  const handleSave = async () => {
    const name = nameDraft.trim();
    if (!name) return;
    setSaving(true);
    setError(null);
    try {
      await coreClient.savePreset({ name, kind, options: currentOptions() });
      setNameDraft("");
      refresh();
    } catch {
      setError("Could not save preset.");
    } finally {
      setSaving(false);
    }
  };

  const handleDelete = async () => {
    if (!selectedId) return;
    try {
      await coreClient.deletePreset(selectedId);
      setSelectedId("");
      refresh();
    } catch {
      // Best-effort.
    }
  };

  return (
    <div className={styles.bar}>
      <select className={styles.select} value={selectedId} onChange={(e) => handleApply(e.target.value)}>
        <option value="">Presets...</option>
        {presets.map((preset) => (
          <option key={preset.id} value={preset.id}>
            {preset.name}
          </option>
        ))}
      </select>
      {selectedId && (
        <button type="button" className={styles.smallButton} onClick={() => void handleDelete()}>
          Delete
        </button>
      )}
      <input
        className={styles.nameInput}
        type="text"
        placeholder="Preset name"
        value={nameDraft}
        onChange={(e) => setNameDraft(e.target.value)}
      />
      <button
        type="button"
        className={styles.smallButton}
        onClick={() => void handleSave()}
        disabled={saving || !nameDraft.trim()}
      >
        {saving ? "Saving..." : "Save preset"}
      </button>
      {error && <span className={styles.error}>{error}</span>}
    </div>
  );
}
