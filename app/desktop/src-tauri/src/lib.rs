mod core_bridge;

use core_bridge::CoreState;
use serde_json::Value;
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

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .setup(|app| {
            let handle = app.handle().clone();
            let state = CoreState::spawn(handle).map_err(|e| -> Box<dyn std::error::Error> { e.into() })?;
            app.manage(state);
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![send_core_command])
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
