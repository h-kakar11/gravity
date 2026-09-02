//! Bridges the mediatool-core sidecar process (stdin/stdout NDJSON, see
//! docs/ipc-contract.md) to Tauri commands and events. This module owns no business
//! logic -- it only frames/deframes JSON lines and routes them by `id` (request/response)
//! or `event` (unsolicited, forwarded verbatim as the "core-event" Tauri event).
//!
//! Spawns the core process with plain `std::process::Command`, not `tauri_plugin_shell`'s
//! sidecar API -- deliberately, so this stays a synchronous stdin-writer /
//! stdout-reader-thread pair like every other process this codebase launches (FFmpeg,
//! yt-dlp), rather than the shell plugin's async `CommandEvent` channel, which would mean
//! rewriting all the I/O plumbing below just to gain sidecar's target-triple path
//! resolution -- a resolution mechanism that isn't even publicly documented (see
//! `resolve_core_path`'s comment). `mediatool-core.exe` is bundled as a plain
//! `bundle.resources` entry instead (Phase 5.2), resolved via the stable, documented
//! `resource_dir()` API.

use std::collections::HashMap;
use std::io::{BufRead, BufReader, Write};
use std::path::PathBuf;
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use serde_json::Value;
use tauri::{AppHandle, Emitter, Manager};
use tokio::sync::oneshot;

/// Requests time out after this long with no matching response line from core.
///
/// Deliberately LONGER than the core's own longest per-command deadline (`kInspectDeadline`
/// in app/core/main.cpp, 30s), and it has to stay that way. Both were 30s, and this timer
/// starts first -- the moment the request is written to stdin, before core has even parsed
/// it -- so this one always expired first. The core's purpose-built E_INSPECT_TIMEOUT /
/// E_INSPECT_PLAYLIST_TIMEOUT errors ("Timed out fetching information about this link. The
/// site may be unreachable or very slow right now.") could therefore never reach the user;
/// what they actually saw was this module's developer-facing string, surfaced by
/// coreClient.ts as a bare E_UNKNOWN.
///
/// This is the outer backstop for a core that has stopped answering at all. The inner,
/// per-command deadlines are what should normally fire, because they know what timed out
/// and can say so.
pub const REQUEST_TIMEOUT: Duration = Duration::from_secs(45);

type PendingMap = Arc<Mutex<HashMap<String, oneshot::Sender<Value>>>>;

/// Tauri-managed state: one per app instance, owns the sidecar child process handle,
/// its stdin, and the table of in-flight request ids awaiting a response.
pub struct CoreState {
    stdin: Mutex<ChildStdin>,
    child: Mutex<Child>,
    pending: PendingMap,
    next_id: AtomicU64,
    /// Flipped to false exactly once, by the stdout reader thread, the moment
    /// mediatool-core's stdout closes (process exited, crashed, or was killed) -- see
    /// issue #23. Checked by `send_request` so a request made after that point fails
    /// immediately with a clear error instead of hitting a raw OS pipe-write error or
    /// silently sitting until `send_core_command`'s REQUEST_TIMEOUT elapses.
    alive: Arc<AtomicBool>,
}

/// Builds the same `{"ok":false,"error":{...}}` envelope shape `send_core_command`
/// (lib.rs) already expects from a real core response, so a synthetic failure requires no
/// special-casing on either the Rust or the frontend side of `send_request`'s callers --
/// `coreClient.ts`'s `toErrorInfo` already knows how to turn this into an `ErrorInfo`.
fn core_unavailable_error(command: &str) -> Value {
    serde_json::json!({
        "ok": false,
        "error": {
            "code": "E_CORE_UNAVAILABLE",
            "category": "ENGINE_FAILURE",
            "message": "Gravity's background engine isn't responding right now.",
            "details": format!("mediatool-core is not running; '{command}' could not be sent"),
            "recoverable": true
        }
    })
}

impl CoreState {
    /// Spawns mediatool-core and starts the background stdout/stderr reader threads.
    /// Must be called once during Tauri's `.setup()` hook, before any command handler runs.
    pub fn spawn(app_handle: AppHandle) -> Result<Self, String> {
        let path = resolve_core_path(&app_handle);
        log::info!("spawning mediatool-core sidecar: {}", path.display());

        let mut command = Command::new(&path);
        command.stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::piped());

        // mediatool-core.exe is a console-subsystem binary; plain std::process::Command
        // shows its console window on Windows unless told not to (unlike reproc, which
        // RealProcessRunner.cpp uses for ffmpeg/yt-dlp/where and which already hides its
        // children's windows internally via STARTF_USESHOWWINDOW/SW_HIDE -- this is the
        // one spawn site in the app that needs the flag explicitly). CREATE_NO_WINDOW =
        // 0x08000000. See issue #37.
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            const CREATE_NO_WINDOW: u32 = 0x08000000;
            command.creation_flags(CREATE_NO_WINDOW);
        }

        // Only set for a packaged build where these bundled resources actually exist
        // (Phase 5.2) -- in dev mode none of these paths exist, so main.cpp's existing
        // MEDIATOOL_*-env-var-with-relative-path-fallback / PATH-search behavior is
        // completely unaffected.
        for (key, value) in resource_relative_env_vars(&app_handle) {
            if value.exists() {
                log::info!("setting {key}={} for mediatool-core", value.display());
                command.env(key, value);
            }
        }

        let mut child = command.spawn().map_err(|e| {
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
        let alive = Arc::new(AtomicBool::new(true));

        spawn_stdout_reader(stdout, pending.clone(), alive.clone(), app_handle);
        spawn_stderr_logger(stderr);

        Ok(CoreState {
            stdin: Mutex::new(stdin),
            child: Mutex::new(child),
            pending,
            next_id: AtomicU64::new(1),
            alive,
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

        if !self.alive.load(Ordering::SeqCst) {
            let (tx, rx) = oneshot::channel::<Value>();
            let _ = tx.send(core_unavailable_error(command));
            return Ok((id, rx));
        }

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
    alive: Arc<AtomicBool>,
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

        // Stdout only closes when the process has exited (crash, kill, or a clean exit we
        // never intentionally trigger outside of CoreState::shutdown) -- this is the
        // earliest and most reliable liveness signal available, so it doubles as the
        // "core died" detector for #23. Every request still waiting on a response gets
        // failed immediately instead of sitting until send_core_command's 30s timeout, and
        // the frontend gets a one-shot event so it can show a real "backend unavailable"
        // state rather than a raw IPC error surfacing wherever the next command happened
        // to be in flight.
        alive.store(false, Ordering::SeqCst);
        let mut pending = pending.lock().unwrap();
        for (_, tx) in pending.drain() {
            let _ = tx.send(core_unavailable_error("(pending request)"));
        }
        drop(pending);
        if let Err(e) = app_handle.emit("core-unavailable", ()) {
            log::warn!("failed to emit core-unavailable to frontend: {e}");
        }
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
/// Phase 5.2: `mediatool-core.exe` is bundled as a plain `bundle.resources` entry (not
/// `externalBin`) specifically to avoid the undocumented target-triple sidecar runtime-path
/// convention -- `resource_dir()` is a stable, documented core-Tauri API, unlike sidecar
/// resolution, which is only officially supported through `tauri-plugin-shell`'s
/// `Command::sidecar()` (a fundamentally different async I/O model this module deliberately
/// doesn't use -- see core_bridge.rs's module doc). If a packaged resource_dir() exists and
/// contains the binary, use it; otherwise fall back to the Phase 1 dev-convenience CWD
/// guess (relative to app/desktop, per docs/development.md), which breaks the moment CWD or
/// the CMake preset differs but was never meant to survive into a packaged build anyway.
fn resolve_core_path(app_handle: &AppHandle) -> PathBuf {
    if let Ok(p) = std::env::var("MEDIATOOL_CORE_PATH") {
        return PathBuf::from(p);
    }

    if let Ok(resource_dir) = app_handle.path().resource_dir() {
        let packaged = resource_dir.join("mediatool-core.exe");
        if packaged.exists() {
            return packaged;
        }
    }

    let cwd = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
    cwd.join("../../build/windows-mingw-debug/app/core/mediatool-core.exe")
}

/// The env vars a packaged build sets on the spawned core process so it finds the bundled
/// ffmpeg/ffprobe/Python resources (Phase 5.2) instead of dev-mode-relative paths --
/// `app/core/main.cpp`'s `EnvOr(...)` calls already read these names, this is just the
/// packaged-runtime side finally supplying real values for that pre-existing seam. Callers
/// are expected to check `.exists()` before setting each one: in dev mode `resource_dir()`
/// either fails or points at a directory none of these files are in, and main.cpp's own
/// relative-path fallbacks must keep working unmodified in that case.
fn resource_relative_env_vars(app_handle: &AppHandle) -> Vec<(&'static str, PathBuf)> {
    let Ok(resource_dir) = app_handle.path().resource_dir() else {
        return Vec::new();
    };

    vec![
        ("MEDIATOOL_FFMPEG_PATH", resource_dir.join("ffmpeg").join("ffmpeg.exe")),
        ("MEDIATOOL_FFPROBE_PATH", resource_dir.join("ffmpeg").join("ffprobe.exe")),
        ("MEDIATOOL_PYTHON_PATH", resource_dir.join("python").join("python.exe")),
        (
            "MEDIATOOL_DOWNLOADER_SCRIPT",
            resource_dir.join("python").join("downloader").join("downloader.py"),
        ),
    ]
}
