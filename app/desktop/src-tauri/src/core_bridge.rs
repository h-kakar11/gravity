//! Bridges the mediatool-core sidecar process (stdin/stdout NDJSON, see
//! docs/ipc-contract.md) to Tauri commands and events. This module owns no business
//! logic -- it only frames/deframes JSON lines and routes them by `id` (request/response)
//! or `event` (unsolicited, forwarded verbatim as the "core-event" Tauri event).

use std::collections::HashMap;
use std::io::{BufRead, BufReader, Write};
use std::path::PathBuf;
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use serde_json::Value;
use tauri::{AppHandle, Emitter};
use tokio::sync::oneshot;

/// Requests time out after this long with no matching response line from core.
pub const REQUEST_TIMEOUT: Duration = Duration::from_secs(30);

type PendingMap = Arc<Mutex<HashMap<String, oneshot::Sender<Value>>>>;

/// Tauri-managed state: one per app instance, owns the sidecar child process handle,
/// its stdin, and the table of in-flight request ids awaiting a response.
pub struct CoreState {
    stdin: Mutex<ChildStdin>,
    child: Mutex<Child>,
    pending: PendingMap,
    next_id: AtomicU64,
}

impl CoreState {
    /// Spawns mediatool-core and starts the background stdout/stderr reader threads.
    /// Must be called once during Tauri's `.setup()` hook, before any command handler runs.
    pub fn spawn(app_handle: AppHandle) -> Result<Self, String> {
        let path = resolve_core_path();
        log::info!("spawning mediatool-core sidecar: {}", path.display());

        let mut child = Command::new(&path)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .map_err(|e| {
                format!(
                    "failed to spawn mediatool-core at {}: {e} (set MEDIATOOL_CORE_PATH if the \
                     Phase 1 dev-mode path guess is wrong)",
                    path.display()
                )
            })?;

        let stdin = child.stdin.take().expect("child stdin was requested as piped");
        let stdout = child.stdout.take().expect("child stdout was requested as piped");
        let stderr = child.stderr.take().expect("child stderr was requested as piped");

        let pending: PendingMap = Arc::new(Mutex::new(HashMap::new()));

        spawn_stdout_reader(stdout, pending.clone(), app_handle);
        spawn_stderr_logger(stderr);

        Ok(CoreState {
            stdin: Mutex::new(stdin),
            child: Mutex::new(child),
            pending,
            next_id: AtomicU64::new(1),
        })
    }

    /// Sends `{"id","command","params"}\n` to core's stdin and registers a oneshot to
    /// receive the eventual matching response line. Returns the raw response envelope
    /// (`{"ok":..,"result"|"error":..}`) -- callers interpret `ok` themselves.
    pub fn send_request(
        &self,
        command: &str,
        params: Value,
    ) -> Result<(String, oneshot::Receiver<Value>), String> {
        let id = format!("req-{}", self.next_id.fetch_add(1, Ordering::SeqCst));

        let (tx, rx) = oneshot::channel::<Value>();
        self.pending.lock().unwrap().insert(id.clone(), tx);

        let request = serde_json::json!({ "id": id, "command": command, "params": params });
        let mut line = serde_json::to_string(&request).map_err(|e| e.to_string())?;
        line.push('\n');

        let write_result = {
            let mut stdin = self.stdin.lock().unwrap();
            stdin
                .write_all(line.as_bytes())
                .and_then(|_| stdin.flush())
        };

        if let Err(e) = write_result {
            self.pending.lock().unwrap().remove(&id);
            return Err(format!("failed to write to mediatool-core stdin: {e}"));
        }

        Ok((id, rx))
    }

    pub fn cancel_pending(&self, id: &str) {
        self.pending.lock().unwrap().remove(id);
    }

    /// Best-effort termination of the sidecar so it doesn't linger as an orphan process.
    pub fn shutdown(&self) {
        if let Ok(mut child) = self.child.lock() {
            log::info!("terminating mediatool-core sidecar on app shutdown");
            let _ = child.kill();
            let _ = child.wait();
        }
    }
}

fn spawn_stdout_reader(
    stdout: std::process::ChildStdout,
    pending: PendingMap,
    app_handle: AppHandle,
) {
    std::thread::spawn(move || {
        let reader = BufReader::new(stdout);
        for line in reader.lines() {
            let line = match line {
                Ok(l) if !l.trim().is_empty() => l,
                Ok(_) => continue,
                Err(e) => {
                    log::warn!("error reading mediatool-core stdout: {e}");
                    break;
                }
            };

            let value: Value = match serde_json::from_str(&line) {
                Ok(v) => v,
                Err(e) => {
                    log::warn!("malformed NDJSON line from mediatool-core ({e}): {line}");
                    continue;
                }
            };

            if let Some(id) = value.get("id").and_then(|v| v.as_str()).map(str::to_owned) {
                let sender = pending.lock().unwrap().remove(&id);
                match sender {
                    Some(tx) => {
                        let _ = tx.send(value);
                    }
                    None => log::warn!("received response for unknown/already-resolved request id: {id}"),
                }
            } else if value.get("event").is_some() {
                // Forward completely unmodified so it matches app/frontend/src/types/ipc.ts's
                // CoreEvent shape field-for-field -- never rename or reshape here.
                if let Err(e) = app_handle.emit("core-event", value) {
                    log::warn!("failed to emit core-event to frontend: {e}");
                }
            } else {
                log::warn!("NDJSON line from mediatool-core has neither 'id' nor 'event': {line}");
            }
        }
        log::info!("mediatool-core stdout closed; reader thread exiting");
    });
}

fn spawn_stderr_logger(stderr: std::process::ChildStderr) {
    std::thread::spawn(move || {
        let reader = BufReader::new(stderr);
        for line in reader.lines().map_while(Result::ok) {
            log::warn!("[mediatool-core stderr] {line}");
        }
    });
}

/// Resolves the sidecar binary path. `MEDIATOOL_CORE_PATH` always wins if set.
///
/// Phase 1 dev-convenience shim only: absent that env var, this guesses the CMake build
/// output relative to the process's current working directory (which is app/desktop when
/// launched via `npm run tauri dev` per docs/development.md). This guess breaks the moment
/// CWD or the CMake preset differs -- real Tauri sidecar bundling (with a
/// target-triple-suffixed binary name, resolved relative to the app resource dir instead of
/// CWD) is a later packaging phase, not this one.
fn resolve_core_path() -> PathBuf {
    if let Ok(p) = std::env::var("MEDIATOOL_CORE_PATH") {
        return PathBuf::from(p);
    }

    let cwd = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
    cwd.join("../../build/windows-mingw-debug/app/core/mediatool-core.exe")
}
