//! System tray + background mode (Phase 4.2, locked decision #2): closing the main
//! window hides it to the tray instead of quitting, gated by the
//! `GeneralSettings::minimizeToTrayOnClose` toggle (added in Phase 3.2's Settings schema
//! ahead of this feature landing, specifically to avoid a second settings migration).
//! Watch Folders (4.1) and Scheduled Tasks (4.3) both depend on this: neither can fire in
//! the background unless the process survives the window closing.

use tauri::{
    menu::{Menu, MenuItem},
    tray::TrayIconBuilder,
    AppHandle, Manager,
};

use crate::core_bridge::CoreState;

const OPEN_MENU_ID: &str = "open";
const QUIT_MENU_ID: &str = "quit";

/// Builds the tray icon and its Open/Quit menu. Called once from `run()`'s `.setup()`.
pub fn setup_tray(app: &tauri::App) -> tauri::Result<()> {
    let open_item = MenuItem::with_id(app, OPEN_MENU_ID, "Open Gravity", true, None::<&str>)?;
    let quit_item = MenuItem::with_id(app, QUIT_MENU_ID, "Quit", true, None::<&str>)?;
    let menu = Menu::with_items(app, &[&open_item, &quit_item])?;

    let icon = app
        .default_window_icon()
        .expect("app icon must be configured in tauri.conf.json")
        .clone();

    TrayIconBuilder::new()
        .icon(icon)
        .menu(&menu)
        .tooltip("Gravity")
        .show_menu_on_left_click(false)
        .on_menu_event(|app, event| match event.id.as_ref() {
            OPEN_MENU_ID => show_main_window(app),
            QUIT_MENU_ID => app.exit(0),
            _ => {}
        })
        .on_tray_icon_event(|tray, event| {
            if let tauri::tray::TrayIconEvent::Click {
                button: tauri::tray::MouseButton::Left,
                button_state: tauri::tray::MouseButtonState::Up,
                ..
            } = event
            {
                show_main_window(tray.app_handle());
            }
        })
        .build(app)?;

    Ok(())
}

fn show_main_window(app: &AppHandle) {
    if let Some(window) = app.get_webview_window("main") {
        let _ = window.show();
        let _ = window.set_focus();
    }
}

/// Reads `GeneralSettings::minimizeToTrayOnClose` via a `getSettings` round-trip through
/// the already-running core sidecar. Deliberately not cached separately: this is a fast
/// local IPC call, and caching would risk drifting from whatever Settings actually holds
/// after an `updateSettings` call -- simplicity over premature optimization. Defaults to
/// `true` (the safer "don't accidentally quit and drop watch-folder/scheduled-task
/// coverage" behavior) if the core process can't be reached or the field is missing.
pub fn should_minimize_to_tray(app: &AppHandle) -> bool {
    let Some(state) = app.try_state::<CoreState>() else {
        return true;
    };

    let response = tauri::async_runtime::block_on(async {
        let (_, rx) = state.send_request("getSettings", serde_json::json!({})).ok()?;
        tokio::time::timeout(std::time::Duration::from_secs(5), rx).await.ok()?.ok()
    });

    response
        .filter(|r| r.get("ok").and_then(|v| v.as_bool()).unwrap_or(false))
        .and_then(|r| r.get("result").cloned())
        .and_then(|result| result.get("settings").cloned())
        .and_then(|settings| settings.get("general").cloned())
        .and_then(|general| general.get("minimizeToTrayOnClose").cloned())
        .and_then(|v| v.as_bool())
        .unwrap_or(true)
}
