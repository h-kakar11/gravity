// Settings (spec section 22): deliberately small. Gravity's settings storage has more fields
// than the backend currently *acts on* -- most of core/settings/Settings.h round-trips to
// disk without changing any runtime behavior yet. Spec section 22 is explicit: "every
// setting must have a real backend effect" and "do not add settings for functionality that
// does not exist" -- so only the fields verified below to change something are editable
// here. See docs/phase-6.md "Settings scope" for the field-by-field audit.
//
// Verified effects:
//  - advanced.ffmpegPath: read at startup to build the FFmpeg engine and the downloader's
//    merge step (app/core/main.cpp AppContext). Editing it changes the *next* launch, which
//    the copy says plainly rather than implying it's immediate.
//  - processing.concurrentJobs: also read at startup, but live control already exists on
//    the Queue screen ("Run at once") and stays in sync with this same setting
//    (HandleSetConcurrency persists back). Shown here read-only rather than as a second,
//    potentially-divergent control for the same value.
//  - general.showNotifications: has no backend consumer, but gates this frontend's own
//    toast notifications (App.tsx) -- a real, immediate, user-visible effect.

import { useEffect, useState } from "react";
import * as coreClient from "../services/coreClient";
import type { Route } from "../components/AppShell";
import type { ErrorInfo } from "../types/error";
import type { Settings } from "../types/settings";
import { asErrorInfo } from "../utils/errors";
import { describeError } from "../utils/jobDisplay";
import { AlertTriangleIcon, CheckCircleIcon, SettingsIcon } from "../components/icons";
import { Button } from "../components/ui/Button";
import { Skeleton } from "../components/ui/Skeleton";

interface SettingsPageProps {
  onNavigate: (route: Route) => void;
  onNotificationsEnabledChange: (enabled: boolean) => void;
}

export default function SettingsPage({ onNavigate, onNotificationsEnabledChange }: SettingsPageProps) {
  const [settings, setSettings] = useState<Settings | null>(null);
  const [error, setError] = useState<ErrorInfo | null>(null);
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState(false);
  const [ffmpegPathDraft, setFfmpegPathDraft] = useState("");

  useEffect(() => {
    let active = true;
    coreClient
      .getSettings()
      .then(({ settings: s }) => {
        if (!active) return;
        setSettings(s);
        setFfmpegPathDraft(s.advanced.ffmpegPath);
      })
      .catch((err) => active && setError(asErrorInfo(err)));
    return () => {
      active = false;
    };
  }, []);

  const save = async (patch: Partial<Settings>) => {
    setSaving(true);
    setError(null);
    setSaved(false);
    try {
      const { settings: updated } = await coreClient.updateSettings(patch);
      setSettings(updated);
      if (patch.general?.showNotifications !== undefined) {
        onNotificationsEnabledChange(updated.general.showNotifications);
      }
      setSaved(true);
      window.setTimeout(() => setSaved(false), 2500);
    } catch (err) {
      setError(asErrorInfo(err));
    } finally {
      setSaving(false);
    }
  };

  if (error && !settings) {
    return (
      <div className="gv-enter">
        <h1 className="gv-h1">Settings</h1>
        <div className="gv-banner gv-banner--error" role="alert">
          <AlertTriangleIcon size={16} />
          <div className="gv-banner__body">
            <div className="gv-banner__title">Couldn't load settings</div>
            <div className="gv-banner__detail">{describeError(error)}</div>
          </div>
        </div>
      </div>
    );
  }

  return (
    <div className="gv-enter">
      <h1 className="gv-h1">Settings</h1>
      <p className="gv-subtitle">
        Only settings that change how Gravity actually behaves are shown here.
      </p>

      {!settings ? (
        <div className="gv-card" style={{ display: "flex", flexDirection: "column", gap: "0.75rem" }}>
          <Skeleton width="40%" />
          <Skeleton width="70%" />
          <Skeleton width="55%" />
        </div>
      ) : (
        <>
          <section style={{ marginBottom: "2rem" }}>
            <h2 className="gv-section-title">General</h2>
            <div className="gv-card">
              <label className="gv-checkbox-row">
                <input
                  type="checkbox"
                  checked={settings.general.showNotifications}
                  onChange={(e) =>
                    void save({ general: { ...settings.general, showNotifications: e.target.checked } })
                  }
                />
                Show notifications for job completion, failure and retries
              </label>
            </div>
          </section>

          <section style={{ marginBottom: "2rem" }}>
            <h2 className="gv-section-title">Queue concurrency</h2>
            <div className="gv-card">
              <p style={{ margin: 0, fontSize: "var(--text-sm)", color: "var(--text-secondary)" }}>
                Currently set to run <strong>{settings.processing.concurrentJobs}</strong> job
                {settings.processing.concurrentJobs === 1 ? "" : "s"} at once. This is controlled
                live from the Queue screen so there is only one place it can be changed.
              </p>
              <div style={{ marginTop: "0.75rem" }}>
                <Button variant="secondary" size="sm" onClick={() => onNavigate("queue")}>
                  Open queue
                </Button>
              </div>
            </div>
          </section>

          <section style={{ marginBottom: "2rem" }}>
            <h2 className="gv-section-title">Advanced</h2>
            <div className="gv-card" style={{ display: "flex", flexDirection: "column", gap: "0.9rem" }}>
              <div className="gv-field">
                <label className="gv-label" htmlFor="ffmpegPath">
                  FFmpeg path
                </label>
                <input
                  id="ffmpegPath"
                  className="gv-input"
                  placeholder="Leave empty to auto-detect"
                  value={ffmpegPathDraft}
                  onChange={(e) => setFfmpegPathDraft(e.target.value)}
                />
                <span className="gv-hint">
                  Applies the next time Gravity starts, not immediately — the media engine is
                  built once at launch.
                </span>
                <div>
                  <Button
                    variant="secondary"
                    size="sm"
                    busy={saving}
                    disabled={ffmpegPathDraft === settings.advanced.ffmpegPath}
                    onClick={() => void save({ advanced: { ...settings.advanced, ffmpegPath: ffmpegPathDraft } })}
                  >
                    Save
                  </Button>
                </div>
              </div>
            </div>
          </section>

          <section>
            <h2 className="gv-section-title">Developer</h2>
            <div className="gv-card">
              <p style={{ margin: "0 0 0.75rem", fontSize: "var(--text-sm)", color: "var(--text-secondary)" }}>
                Raw IPC inspection tools used while building Gravity. Not part of the normal
                workflow.
              </p>
              <Button variant="ghost" size="sm" icon={<SettingsIcon size={14} />} onClick={() => onNavigate("devConsole")}>
                Open developer console
              </Button>
            </div>
          </section>

          {error ? (
            <div className="gv-banner gv-banner--error" role="alert" style={{ marginTop: "1.5rem" }}>
              <AlertTriangleIcon size={16} />
              <div className="gv-banner__body">
                <div className="gv-banner__title">Couldn't save settings</div>
                <div className="gv-banner__detail">{describeError(error)}</div>
              </div>
            </div>
          ) : null}
          {saved ? (
            <div className="gv-banner gv-banner--success" role="status" style={{ marginTop: "1.5rem" }}>
              <CheckCircleIcon size={16} />
              <div className="gv-banner__body">
                <div className="gv-banner__title">Saved</div>
              </div>
            </div>
          ) : null}
        </>
      )}
    </div>
  );
}
