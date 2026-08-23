import { useCallback, useEffect, useMemo, useState, type CSSProperties } from "react";
import { getCurrentWebview } from "@tauri-apps/api/webview";
import { useJobs } from "../hooks/useJobs";
import * as coreClient from "../services/coreClient";
import type { FileInfo } from "../types/fileInfo";
import type { HardwareInfo } from "../types/hardware";
import type { Settings } from "../types/settings";
import type { ErrorInfo } from "../types/error";
import type { JobState } from "../types/job";

// TODO(spec section 34): replace this plain dev console with the dark-mode, curved-card
// premium home screen once the IPC pipeline it proves out (spec sections 33, 42) is solid.

function asErrorInfo(err: unknown): ErrorInfo {
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

const CANCELLABLE_STATES: ReadonlySet<JobState> = new Set(["QUEUED", "STARTING", "RUNNING", "PAUSED"]);

function ErrorBanner({ error }: { error: ErrorInfo | null }) {
  if (!error) return null;
  return (
    <div style={styles.errorBanner}>
      <strong>{error.category}</strong> ({error.code}): {error.message}
      {error.details && error.details !== error.message ? (
        <div style={styles.errorDetails}>{error.details}</div>
      ) : null}
    </div>
  );
}

function JsonBlock({ value }: { value: unknown }) {
  return <pre style={styles.jsonBlock}>{JSON.stringify(value, null, 2)}</pre>;
}

export default function DevConsole() {
  const { jobs, connectionError, createTestJob, cancelJob } = useJobs();

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

  const testJob = useMemo(() => jobs.find((j) => j.id === testJobId) ?? null, [jobs, testJobId]);

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
    // Tauri v2 native drag & drop: the window's "tauri://drag-drop" event, exposed here via
    // the webview module's onDragDropEvent. Deliberately NOT the HTML5 DataTransfer API --
    // only this Tauri-native event carries a real filesystem path for the dropped file.
    // Requires a matching drag-drop capability entry for this window in the Rust shell's
    // tauri.conf.json / src-tauri/capabilities/*.json, or the event never fires (see summary).
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
      const jobId = await createTestJob();
      setTestJobId(jobId);
    } catch (err) {
      setSelfTestError(asErrorInfo(err));
    } finally {
      setSelfTestBusy(false);
    }
  }, [createTestJob]);

  const handleCancelTestJob = useCallback(async () => {
    if (!testJobId) return;
    setSelfTestBusy(true);
    setSelfTestError(null);
    try {
      await cancelJob(testJobId);
    } catch (err) {
      setSelfTestError(asErrorInfo(err));
    } finally {
      setSelfTestBusy(false);
    }
  }, [testJobId, cancelJob]);

  const canCancelTestJob = testJob !== null && CANCELLABLE_STATES.has(testJob.state);

  return (
    <div style={styles.page}>
      <h1 style={styles.h1}>MediaTool Dev Console</h1>
      <p style={styles.subtitle}>
        Phase 1 IPC proving ground -- not the final UI (spec section 34). Exercises the
        React &lt;-&gt; Rust &lt;-&gt; C++ core pipeline end-to-end (spec sections 33, 42).
      </p>

      {connectionError ? (
        <div style={styles.errorBanner}>
          <strong>Core process not reachable:</strong> {connectionError}
        </div>
      ) : (
        <div style={styles.okBanner}>Core process reachable.</div>
      )}

      <section style={styles.section}>
        <h2 style={styles.h2}>Self-Test Job</h2>
        <div style={styles.row}>
          <button onClick={() => void runSelfTest()} disabled={selfTestBusy}>
            Run Self-Test
          </button>
          <button onClick={() => void handleCancelTestJob()} disabled={!canCancelTestJob || selfTestBusy}>
            Cancel
          </button>
        </div>
        <ErrorBanner error={selfTestError} />
        {testJob ? (
          <div style={styles.card}>
            <div>
              job <code>{testJob.id}</code> -- state <strong>{testJob.state}</strong>
            </div>
            <div>percentage: {testJob.progress.percentage ?? "n/a"}</div>
            <div>status: {testJob.progress.statusMessage}</div>
            {testJob.error ? <ErrorBanner error={testJob.error} /> : null}
          </div>
        ) : (
          <p style={styles.muted}>No self-test job started yet.</p>
        )}
      </section>

      <section style={styles.section}>
        <h2 style={styles.h2}>Drop a File</h2>
        <div style={isDragActive ? styles.dropZoneActive : styles.dropZone}>
          Drag a file from Windows Explorer onto this window.
          {droppedPath ? <div style={styles.muted}>Last dropped: {droppedPath}</div> : null}
        </div>
        <ErrorBanner error={dropError} />
        {droppedFileInfo ? (
          <div style={styles.row}>
            <div style={styles.card}>
              <h3 style={styles.h3}>inspectFile result</h3>
              <JsonBlock value={droppedFileInfo} />
            </div>
            <div style={styles.card}>
              <h3 style={styles.h3}>getCapabilities result</h3>
              <JsonBlock value={droppedCapabilities} />
            </div>
          </div>
        ) : null}
      </section>

      <section style={styles.section}>
        <h2 style={styles.h2}>Settings &amp; Hardware</h2>
        <ErrorBanner error={panelError} />
        <div style={styles.row}>
          <div style={styles.card}>
            <h3 style={styles.h3}>getSettings</h3>
            {settings ? <JsonBlock value={settings} /> : <p style={styles.muted}>Loading...</p>}
          </div>
          <div style={styles.card}>
            <h3 style={styles.h3}>getHardwareInfo</h3>
            {hardwareInfo ? <JsonBlock value={hardwareInfo} /> : <p style={styles.muted}>Loading...</p>}
          </div>
        </div>
      </section>

      <section style={styles.section}>
        <h2 style={styles.h2}>All Jobs ({jobs.length})</h2>
        <JsonBlock value={jobs} />
      </section>
    </div>
  );
}

const styles: Record<string, CSSProperties> = {
  page: {
    fontFamily: "system-ui, -apple-system, Segoe UI, sans-serif",
    maxWidth: 960,
    margin: "0 auto",
    padding: "1.5rem",
    display: "flex",
    flexDirection: "column",
    gap: "1rem",
  },
  h1: { fontSize: "1.5rem", margin: 0 },
  h2: { fontSize: "1.1rem", margin: "0 0 0.5rem 0" },
  h3: { fontSize: "0.95rem", margin: "0 0 0.5rem 0" },
  subtitle: { color: "#555", marginTop: 0 },
  section: {
    border: "1px solid #ddd",
    borderRadius: 8,
    padding: "1rem",
    display: "flex",
    flexDirection: "column",
    gap: "0.5rem",
  },
  row: { display: "flex", gap: "1rem", flexWrap: "wrap" },
  card: {
    flex: "1 1 300px",
    border: "1px solid #eee",
    borderRadius: 6,
    padding: "0.75rem",
    background: "#fafafa",
  },
  jsonBlock: {
    margin: 0,
    fontSize: "0.8rem",
    overflowX: "auto",
    whiteSpace: "pre",
    maxHeight: 300,
    overflowY: "auto",
  },
  dropZone: {
    border: "2px dashed #bbb",
    borderRadius: 8,
    padding: "2rem",
    textAlign: "center",
    color: "#666",
  },
  dropZoneActive: {
    border: "2px dashed #3b82f6",
    borderRadius: 8,
    padding: "2rem",
    textAlign: "center",
    color: "#1d4ed8",
    background: "#eff6ff",
  },
  errorBanner: {
    border: "1px solid #f5c2c7",
    background: "#f8d7da",
    color: "#842029",
    borderRadius: 6,
    padding: "0.5rem 0.75rem",
    fontSize: "0.9rem",
  },
  errorDetails: { fontSize: "0.8rem", marginTop: "0.25rem", whiteSpace: "pre-wrap" },
  okBanner: {
    border: "1px solid #badbcc",
    background: "#d1e7dd",
    color: "#0f5132",
    borderRadius: 6,
    padding: "0.5rem 0.75rem",
    fontSize: "0.9rem",
  },
  muted: { color: "#888", fontSize: "0.85rem" },
};
