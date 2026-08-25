mod core_bridge;
mod hotkeys;
mod paths;
mod scheduler;
mod tray;
mod watch_folders;

use core_bridge::CoreState;
use serde_json::Value;
use std::path::Path;
use std::process::Command;
use tauri::Manager;

/// The one Tauri command the frontend uses to talk to mediatool-core (see
/// app/frontend/src/types/ipc.ts's `CoreCommand`/`CommandParams`/`CommandResult`).
/// `command` and `params` are forwarded to core as-is; this function only handles
/// request-id bookkeeping, the timeout, and unwrapping `ok`/`result`/`error`.
#[tauri::command]
async fn send_core_command(
    command: String,
    params: Value,
    state: tauri::State<'_, CoreState>,
) -> Result<Value, String> {
    let (id, rx) = state.send_request(&command, params)?;

    let response = match tokio::time::timeout(core_bridge::REQUEST_TIMEOUT, rx).await {
        Ok(Ok(response)) => response,
        Ok(Err(_)) => {
            state.cancel_pending(&id);
            return Err(format!(
                "mediatool-core closed its response channel before replying to '{command}'"
            ));
        }
        Err(_) => {
            state.cancel_pending(&id);
            return Err(format!(
                "timed out after {:?} waiting for mediatool-core to respond to '{command}'",
                core_bridge::REQUEST_TIMEOUT
            ));
        }
    };

    let ok = response.get("ok").and_then(Value::as_bool).unwrap_or(false);
    if ok {
        Ok(response.get("result").cloned().unwrap_or(Value::Null))
    } else {
        let error = response.get("error").cloned().unwrap_or_else(|| {
            serde_json::json!({
                "code": "E_MALFORMED_RESPONSE",
                "category": "UNKNOWN",
                "message": "mediatool-core returned ok:false with no error object",
                "details": response.to_string(),
                "recoverable": false
            })
        });
        // Stringified JSON so the frontend can JSON.parse() it back into an ErrorInfo
        // (docs/ipc-contract.md) -- Tauri command errors only carry a String across the bridge.
        Err(error.to_string())
    }
}

/// Reveals a completed download's output file in Windows Explorer (spec section 37: "open
/// containing folder", implemented through the backend rather than an arbitrary shell
/// command from React). `/select,<path>` is one argv entry passed straight to explorer.exe
/// -- no shell string concatenation, same "structured arguments only" rule the C++ core
/// follows for every process it launches.
#[tauri::command]
fn open_containing_folder(path: String) -> Result<(), String> {
    if !Path::new(&path).exists() {
        return Err(format!("cannot open containing folder: path does not exist: {path}"));
    }
    Command::new("explorer")
        .arg(format!("/select,{path}"))
        .spawn()
        .map(|_| ())
        .map_err(|e| format!("failed to launch explorer.exe: {e}"))
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_global_shortcut::Builder::new().build())
        .plugin(tauri_plugin_clipboard_manager::init())
        .plugin(tauri_plugin_notification::init())
        .setup(|app| {
            let handle = app.handle().clone();
            let state = CoreState::spawn(handle).map_err(|e| -> Box<dyn std::error::Error> { e.into() })?;
            app.manage(state);

            tray::setup_tray(app)?;

            app.manage(watch_folders::WatchFolderState::default());
            watch_folders::restore_watch_folders(&app.handle().clone());

            app.manage(scheduler::ScheduledTaskState::default());
            scheduler::start_scheduler(&app.handle().clone());

            if let Err(e) = hotkeys::refresh_hotkeys(app.handle().clone()) {
                log::warn!("failed to register global hotkeys: {e}");
            }

            // Background mode (#2): intercept the main window's close request and hide
            // to the tray instead of quitting, unless the user has opted out via
            // Settings -- see tray::should_minimize_to_tray. Watch Folders (4.1) and
            // Scheduled Tasks (4.3) both need the process to survive past this point.
            if let Some(window) = app.get_webview_window("main") {
                let app_handle = app.handle().clone();
                window.on_window_event(move |event| {
                    if let tauri::WindowEvent::CloseRequested { api, .. } = event {
                        if tray::should_minimize_to_tray(&app_handle) {
                            api.prevent_close();
                            if let Some(window) = app_handle.get_webview_window("main") {
                                let _ = window.hide();
                            }
                        }
                    }
                });
            }

            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            send_core_command,
            open_containing_folder,
            watch_folders::add_watch_folder,
            watch_folders::remove_watch_folder,
            watch_folders::list_watch_folders,
            scheduler::add_scheduled_task,
            scheduler::update_scheduled_task,
            scheduler::remove_scheduled_task,
            scheduler::list_scheduled_tasks,
            hotkeys::refresh_hotkeys
        ])
        .build(tauri::generate_context!())
        .expect("error while building the MediaTool Tauri application")
        .run(|app_handle, event| {
            // Best-effort cleanup: kill the sidecar so it doesn't linger as an orphan
            // process. Phase 1 gap: this only fires on a normal app exit event, not on a
            // hard kill of the Tauri process itself (e.g. Task Manager "End Task") -- a
            // fully robust job-object-based cleanup is left for a later packaging pass.
            if let tauri::RunEvent::Exit = event {
                if let Some(state) = app_handle.try_state::<CoreState>() {
                    state.shutdown();
                }
            }
        });
}
