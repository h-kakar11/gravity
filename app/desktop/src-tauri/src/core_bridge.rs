//! Bridges the mediatool-core sidecar process (stdin/stdout NDJSON, see
//! docs/ipc-contract.md) to Tauri commands and events. This module owns no business
//! logic -- it only frames/deframes JSON lines and routes them by `id` (request/response)
//! or `event` (unsolicited, forwarded verbatim as the "core-event" Tauri event).

use std::collections::HashMap;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use serde_json::Value;
use tauri::{AppHandle, Emitter, Manager};
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
        // Tauri is the one piece of this application that actually knows where a packaged
        // install placed its bundled resources (it wrote tauri.conf.json's `bundle.resources`
        // there in the first place) -- so it resolves that location once, here, and hands it
        // to the child explicitly via MEDIATOOL_RESOURCE_DIR rather than letting mediatool-core
        // re-derive or guess it (Phase 7, "no CWD dependency"). See docs/phase-7.md
        // "Resource discovery" for the full strategy and core/filesystem/ExecutablePath.h for
        // the C++ side's fallback when this isn't set (e.g. running the core binary directly).
        let resource_dir = app_handle.path().resource_dir().ok();
        if let Some(dir) = &resource_dir {
            log::info!("resolved app resource directory: {}", dir.display());
        } else {
            log::warn!(
                "could not resolve the app resource directory (expected in `tauri dev`); \
                 mediatool-core will fall back to its own executable-relative defaults"
            );
        }

        let path = resolve_core_path(resource_dir.as_deref());
        log::info!("spawning mediatool-core sidecar: {}", path.display());

        let mut command = Command::new(&path);
        command.stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::piped());
        if let Some(dir) = &resource_dir {
            command.env("MEDIATOOL_RESOURCE_DIR", dir);
        }

        let mut child = command.spawn().map_err(|e| {
            format!(
                "failed to spawn mediatool-core at {}: {e} (set MEDIATOOL_CORE_PATH to override \
                 the resolved path, e.g. for local development)",
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

/// Resolves the sidecar binary path, in order:
///   1. `MEDIATOOL_CORE_PATH` -- always wins if set (development override).
///   2. `<resource_dir>/mediatool-core.exe` -- a packaged install's own bundled copy,
///      never anything the user's environment happens to provide.
///   3. A CMake-build-relative guess, kept only as a last resort for local development
///      when neither of the above apply (e.g. `cargo run` with no resource dir resolved
///      and no override set) -- this is CWD-dependent by nature and is not what a
///      packaged install uses.
fn resolve_core_path(resource_dir: Option<&Path>) -> PathBuf {
    if let Ok(p) = std::env::var("MEDIATOOL_CORE_PATH") {
        return PathBuf::from(p);
    }

    if let Some(dir) = resource_dir {
        let bundled = dir.join(core_binary_name());
        if bundled.is_file() {
            return bundled;
        }
    }

    let cwd = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
    cwd.join("../../build/windows-mingw-debug/app/core").join(core_binary_name())
}

#[cfg(target_os = "windows")]
fn core_binary_name() -> &'static str {
    "mediatool-core.exe"
}

#[cfg(not(target_os = "windows"))]
fn core_binary_name() -> &'static str {
    "mediatool-core"
}
