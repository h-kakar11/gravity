//! Global Hotkeys (Phase 4.4): reads `GeneralSettings::hotkeyPasteAndDownload` /
//! `hotkeyFocusQueue` from the C++-validated `Settings` struct (core/settings/Settings.h)
//! -- deliberately not a fourth ad hoc Rust JSON config file, so the bindings get the same
//! validation-on-write every other setting gets -- and registers them as OS-level global
//! shortcuts via tauri-plugin-global-shortcut.
//!
//! "Paste link and download" reads the clipboard (tauri-plugin-clipboard-manager) and emits
//! a Tauri event carrying the text rather than calling `createJob` directly the way Watch
//! Folders/Scheduled Tasks do: the user needs to see and confirm what's about to download,
//! not have it fire silently from a keystroke.
//!
//! `refresh_hotkeys` is called once from `run()`'s `.setup()` and again by the frontend
//! (as a Tauri command) after any `updateSettings` call that could have changed a binding.

use tauri::{AppHandle, Emitter, Manager};
use tauri_plugin_clipboard_manager::ClipboardExt;
use tauri_plugin_global_shortcut::{GlobalShortcutExt, ShortcutState};

use crate::core_bridge::CoreState;
use crate::tray::show_main_window;

const PASTE_AND_DOWNLOAD_EVENT: &str = "hotkey-paste-and-download";
const FOCUS_QUEUE_EVENT: &str = "hotkey-focus-queue";

/// (Re-)registers both hotkeys from the current Settings, replacing whatever was previously
/// registered. Always starts from `unregister_all()` so a changed or cleared binding never
/// leaves a stale one active. A no-op (not an error) if the core process isn't reachable yet
/// -- `run()`'s `.setup()` calls this after `CoreState` is already managed, so that only
/// matters for a future call racing a slow core startup.
#[tauri::command]
pub fn refresh_hotkeys(app: AppHandle) -> Result<(), String> {
    let shortcuts = app.global_shortcut();
    shortcuts.unregister_all().map_err(|e| e.to_string())?;

    let Some(state) = app.try_state::<CoreState>() else {
        return Ok(());
    };
    let Some(general) = fetch_general_settings(&state) else {
        return Ok(());
    };

    if let Some(binding) = non_empty(general.get("hotkeyPasteAndDownload")) {
        let app_handle = app.clone();
        if let Err(e) = shortcuts.on_shortcut(binding.as_str(), move |_app, _shortcut, event| {
            if event.state() == ShortcutState::Pressed {
                handle_paste_and_download(&app_handle);
            }
        }) {
            log::warn!("failed to register paste-and-download hotkey '{binding}': {e}");
        }
    }

    if let Some(binding) = non_empty(general.get("hotkeyFocusQueue")) {
        let app_handle = app.clone();
        if let Err(e) = shortcuts.on_shortcut(binding.as_str(), move |_app, _shortcut, event| {
            if event.state() == ShortcutState::Pressed {
                show_main_window(&app_handle);
                let _ = app_handle.emit(FOCUS_QUEUE_EVENT, ());
            }
        }) {
            log::warn!("failed to register focus-queue hotkey '{binding}': {e}");
        }
    }

    Ok(())
}

fn handle_paste_and_download(app: &AppHandle) {
    let clipboard_text = match app.clipboard().read_text() {
        Ok(text) if !text.trim().is_empty() => text,
        _ => return,
    };
    show_main_window(app);
    let _ = app.emit(
        PASTE_AND_DOWNLOAD_EVENT,
        serde_json::json!({ "url": clipboard_text.trim() }),
    );
}

fn non_empty(value: Option<&serde_json::Value>) -> Option<String> {
    value?
        .as_str()
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .map(str::to_owned)
}

/// Mirrors `tray::should_minimize_to_tray`'s shape: a `getSettings` round-trip through the
/// already-running core sidecar, unwrapped down to just the `general` section.
fn fetch_general_settings(state: &CoreState) -> Option<serde_json::Value> {
    let response = tauri::async_runtime::block_on(async {
        let (_, rx) = state.send_request("getSettings", serde_json::json!({})).ok()?;
        tokio::time::timeout(std::time::Duration::from_secs(5), rx).await.ok()?.ok()
    });

    response
        .filter(|r| r.get("ok").and_then(|v| v.as_bool()).unwrap_or(false))
        .and_then(|r| r.get("result").cloned())
        .and_then(|result| result.get("settings").cloned())
        .and_then(|settings| settings.get("general").cloned())
}
