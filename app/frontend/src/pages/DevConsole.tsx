// Diagnostics screen (spec section 4: reachable only from Settings > Developer, not primary
// navigation). Exercises the IPC pipeline directly -- self-test job, native drag & drop,
// settings/hardware round trip -- for debugging, not as a product screen.
//
// Reads jobs from the same queue store as every other screen (App.tsx's single useQueue()
// instance) instead of opening its own subscription, so this and the Queue page can never
// show two different ideas of a job's state.

import { useCallback, useEffect, useState } from "react";
import { getCurrentWebview } from "@tauri-apps/api/webview";
import * as coreClient from "../services/coreClient";
import type { FileInfo } from "../types/fileInfo";
import type { HardwareInfo } from "../types/hardware";
import type { Settings } from "../types/settings";
import type { ErrorInfo } from "../types/error";
import { asErrorInfo } from "../utils/errors";
import { describeError } from "../utils/jobDisplay";
import type { QueueController } from "../state/useQueue";
import { AlertTriangleIcon } from "../components/icons";
import { Button } from "../components/ui/Button";

function ErrorBanner({ error }: { error: ErrorInfo | null }) {
  if (!error) return null;
  return (
    <div className="gv-banner gv-banner--error" role="alert">
      <AlertTriangleIcon size={15} />
      <div className="gv-banner__body">
        <div className="gv-banner__title">{describeError(error)}</div>
        {error.details && error.details !== error.message ? (
          <div className="gv-banner__detail">{error.details}</div>
        ) : null}
      </div>
    </div>
  );
}

function JsonBlock({ value }: { value: unknown }) {
  return (
    <pre style={{ margin: 0, fontSize: "0.75rem", overflow: "auto", maxHeight: 280 }}>
      {JSON.stringify(value, null, 2)}
    </pre>
  );
}

export default function DevConsole({ queue }: { queue: QueueController }) {
  const [testJobId, setTestJobId] = useState<string | null>(null);
  const [selfTestBusy, setSelfTestBusy] = useState(false);
  const [selfTestError, setSelfTestError] = useState<ErrorInfo | null>(null);

  const [isDragActive, setIsDragActive] = useState(false);
  const [droppedPath, setDroppedPath] = useState<string | null>(null);
  const [droppedFileInfo, setDroppedFileInfo] = useState<FileInfo | null>(null);
  const [droppedCapabilities, setDroppedCapabilities] = useState<string[] | null>(null);
  const [dropError, setDropError] = useState<ErrorInfo | null>(null);

  const [settings, setSettings] = useState<Settings | null>(null);
  const [hardwareInfo, setHardwareInfo] = useState<HardwareInfo | null>(null);
  const [panelError, setPanelError] = useState<ErrorInfo | null>(null);

  const testJob = testJobId ? queue.state.jobs[testJobId] : undefined;

  useEffect(() => {
    coreClient
      .getSettings()
      .then(({ settings: s }) => setSettings(s))
      .catch((err) => setPanelError(asErrorInfo(err)));
    coreClient
      .getHardwareInfo()
      .then(({ hardwareInfo: h }) => setHardwareInfo(h))
      .catch((err) => setPanelError(asErrorInfo(err)));
  }, []);

  const handleDroppedPath = useCallback(async (path: string) => {
    setDroppedPath(path);
    setDropError(null);
    setDroppedFileInfo(null);
    setDroppedCapabilities(null);
    try {
      const [{ fileInfo }, { capabilities }] = await Promise.all([
        coreClient.inspectFile(path),
        coreClient.getCapabilities(path),
      ]);
      setDroppedFileInfo(fileInfo);
      setDroppedCapabilities(capabilities);
    } catch (err) {
      setDropError(asErrorInfo(err));
    }
  }, []);

  useEffect(() => {
    let unlisten: (() => void) | undefined;
    let cancelled = false;

    getCurrentWebview()
      .onDragDropEvent((event) => {
        switch (event.payload.type) {
          case "enter":
          case "over":
            setIsDragActive(true);
            break;
          case "drop":
            setIsDragActive(false);
            if (event.payload.paths[0]) void handleDroppedPath(event.payload.paths[0]);
            break;
          case "leave":
            setIsDragActive(false);
            break;
        }
      })
      .then((fn) => {
        if (cancelled) fn();
        else unlisten = fn;
      });

    return () => {
      cancelled = true;
      unlisten?.();
    };
  }, [handleDroppedPath]);

  const runSelfTest = useCallback(async () => {
    setSelfTestBusy(true);
    setSelfTestError(null);
    try {
      const { jobId } = await coreClient.createJob({ type: "TEST", params: {} });
      setTestJobId(jobId);
    } catch (err) {
      setSelfTestError(asErrorInfo(err));
    } finally {
      setSelfTestBusy(false);
    }
  }, []);

  const canCancelTestJob = testJob !== undefined && queue.jobs.some((j) => j.id === testJob.id) && ["QUEUED", "STARTING", "RUNNING"].includes(testJob.state);

  return (
    <div className="gv-enter">
      <h1 className="gv-h1">Developer console</h1>
      <p className="gv-subtitle">
        Raw IPC inspection tools used while building Gravity. Not part of the normal workflow
        (reached only from Settings).
      </p>

      {!queue.state.loaded ? (
        <div className="gv-banner gv-banner--error" role="alert">
          <AlertTriangleIcon size={15} />
          <div className="gv-banner__body">
            <div className="gv-banner__title">Core process not reachable</div>
          </div>
        </div>
      ) : (
        <div className="gv-banner gv-banner--success" role="status">
          Core process reachable.
        </div>
      )}

      <section className="gv-card" style={{ marginBottom: "1.25rem" }}>
        <h2 className="gv-section-title">Self-test job</h2>
        <div style={{ display: "flex", gap: "0.5rem", marginBottom: "0.5rem" }}>
          <Button variant="secondary" size="sm" busy={selfTestBusy} onClick={() => void runSelfTest()}>
            Run self-test
          </Button>
          <Button
            variant="destructive"
            size="sm"
            disabled={!canCancelTestJob}
            onClick={() => testJob && void queue.cancelJob(testJob.id)}
          >
            Cancel
          </Button>
        </div>
        <ErrorBanner error={selfTestError} />
        {testJob ? (
          <div style={{ fontSize: "0.85rem" }}>
            <div>
              job <code>{testJob.id}</code> — state <strong>{testJob.state}</strong>
            </div>
            <div>percentage: {testJob.progress.percentage ?? "n/a"}</div>
            <div>status: {testJob.progress.statusMessage}</div>
            {testJob.error ? <ErrorBanner error={testJob.error} /> : null}
          </div>
        ) : (
          <p className="gv-hint">No self-test job started yet.</p>
        )}
      </section>

      <section className="gv-card" style={{ marginBottom: "1.25rem" }}>
        <h2 className="gv-section-title">Drop a file</h2>
        <div
          style={{
            border: `2px dashed ${isDragActive ? "var(--accent)" : "var(--border-default)"}`,
            borderRadius: 8,
            padding: "2rem",
            textAlign: "center",
            color: "var(--text-secondary)",
          }}
        >
          Drag a file onto this window.
          {droppedPath ? <div className="gv-hint">Last dropped: {droppedPath}</div> : null}
        </div>
        <ErrorBanner error={dropError} />
        {droppedFileInfo ? (
          <div className="gv-grid-2" style={{ marginTop: "0.75rem" }}>
            <div>
              <h3 style={{ fontSize: "0.85rem" }}>inspectFile result</h3>
              <JsonBlock value={droppedFileInfo} />
            </div>
            <div>
              <h3 style={{ fontSize: "0.85rem" }}>getCapabilities result</h3>
              <JsonBlock value={droppedCapabilities} />
            </div>
          </div>
        ) : null}
      </section>

      <section className="gv-card" style={{ marginBottom: "1.25rem" }}>
        <h2 className="gv-section-title">Settings &amp; hardware</h2>
        <ErrorBanner error={panelError} />
        <div className="gv-grid-2">
          <div>
            <h3 style={{ fontSize: "0.85rem" }}>getSettings</h3>
            {settings ? <JsonBlock value={settings} /> : <p className="gv-hint">Loading…</p>}
          </div>
          <div>
            <h3 style={{ fontSize: "0.85rem" }}>getHardwareInfo</h3>
            {hardwareInfo ? <JsonBlock value={hardwareInfo} /> : <p className="gv-hint">Loading…</p>}
          </div>
        </div>
      </section>

      <section className="gv-card">
        <h2 className="gv-section-title">All jobs ({queue.jobs.length})</h2>
        <JsonBlock value={queue.jobs} />
      </section>
    </div>
  );
}
