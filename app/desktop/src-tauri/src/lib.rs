mod cli;
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

/// The `/select,` argument for explorer.exe, as a single raw command-line fragment.
///
/// Two things have to be true at once and they pull against each other:
///
///  - explorer.exe parses `/select,<path>` with its own undocumented comma/quote rules
///    rather than standard argv splitting, so an unquoted path containing a comma can
///    select the wrong item -- issue #38, which is why the quotes are here. `"` is illegal
///    in a Windows filename (see the C++ side's FilenameSanitizer reserved-character set),
///    so the path itself can never need escaping.
///  - those quotes must reach explorer.exe **verbatim**, which `Command::arg` cannot do:
///    Rust's Windows argument encoder escapes every `"` in a regular argument
///    unconditionally (`append_arg` in std's `sys::args::windows` inserts a backslash
///    before each one whether or not the argument itself gets quoted). So `Command::arg`
///    turned `/select,"C:\out\clip.mp4"` into a command line reading
///    `/select,\"C:\out\clip.mp4\"`, explorer read the path as `\"C:\out\clip.mp4\"`,
///    and it could not find that file -- issue #84. The #38 fix defeated itself.
///
/// `raw_arg` (used by the caller) is the escape hatch for exactly this: it appends the
/// fragment to the command line with no quoting or escaping at all.
///
/// Separators are normalized to backslashes because explorer.exe will not select through a
/// forward-slash path, and an output directory typed into the UI as `D:/Converted` reaches
/// here exactly as typed.
fn explorer_select_argument(path: &str) -> String {
    format!("/select,\"{}\"", path.replace('/', "\\"))
}

/// Reveals a completed job's output file in Windows Explorer (spec section 37: "open
/// containing folder", implemented through the backend rather than an arbitrary shell
/// command from React).
#[tauri::command]
fn open_containing_folder(path: String) -> Result<(), String> {
    if !Path::new(&path).exists() {
        return Err(format!("cannot open containing folder: path does not exist: {path}"));
    }
    let argument = explorer_select_argument(&path);

    let mut command = Command::new("explorer");
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        // See explorer_select_argument: the quotes must survive to explorer.exe intact.
        command.raw_arg(&argument);
    }
    #[cfg(not(windows))]
    {
        // Gravity is Windows-only; this branch exists so the crate still compiles (and its
        // tests still run) on a non-Windows host, where `raw_arg` doesn't exist.
        command.arg(&argument);
    }

    command
        .spawn()
        .map(|_| ())
        .map_err(|e| format!("failed to launch explorer.exe: {e}"))
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        // Registered first, per tauri-plugin-single-instance's own requirement: plugins run
        // in registration order, and this one needs to intercept a second launch before
        // anything else in the chain reacts to app startup. See src/cli.rs -- the
        // already-running-instance half of the Phase 5.3 CLI contract.
        .plugin(tauri_plugin_single_instance::init(|app, args, _cwd| {
            cli::handle_second_instance(app, &args);
        }))
        .plugin(tauri_plugin_global_shortcut::Builder::new().build())
        .plugin(tauri_plugin_clipboard_manager::init())
        .plugin(tauri_plugin_notification::init())
        .plugin(tauri_plugin_dialog::init())
        .setup(|app| {
            app.manage(cli::CliState::default());
            // Cold-start half of the Phase 5.3 CLI contract -- the frontend isn't mounted
            // yet, so this stashes the parsed action rather than emitting an event (see
            // cli.rs's module doc for why).
            cli::store_startup_action(&app.handle().clone(), &std::env::args().collect::<Vec<_>>());

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
            hotkeys::refresh_hotkeys,
            cli::get_startup_file_action
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn explorer_argument_keeps_the_quotes_that_protect_a_comma_in_the_path() {
        // Issue #38: explorer.exe splits `/select,<path>` on its own rules, so a comma in
        // the filename selects the wrong item unless the path portion is quoted.
        assert_eq!(
            explorer_select_argument("C:\\out\\clip, final.mp4"),
            "/select,\"C:\\out\\clip, final.mp4\""
        );
    }

    #[test]
    fn explorer_argument_normalizes_forward_slashes_to_backslashes() {
        // An output directory typed into the UI as `D:/Converted` reaches this function
        // exactly as typed, and explorer.exe will not select through a forward-slash path.
        assert_eq!(
            explorer_select_argument("D:/Converted/clip.mp4"),
            "/select,\"D:\\Converted\\clip.mp4\""
        );
    }

    #[test]
    fn explorer_argument_is_a_single_fragment_with_no_escaping_of_its_own() {
        // Issue #84: the quotes have to reach explorer.exe verbatim. This function must
        // therefore produce the exact bytes to append -- any backslash-escaping of the
        // quotes belongs nowhere, here or in the spawn (which uses raw_arg for that
        // reason). A `\"` appearing in this string would be the bug.
        let argument = explorer_select_argument("C:\\out\\clip.mp4");
        assert!(!argument.contains("\\\""), "must not escape its own quotes: {argument}");
        assert_eq!(argument.matches('"').count(), 2);
    }

    /// Issue #85. The NSIS context-menu hooks must never hardcode the installed
    /// executable's filename: Tauri derives it from `mainBinaryName`, which defaults to the
    /// Cargo package name (`gravity-desktop`), NOT to `productName` (`Gravity`). Pointing a
    /// shell verb at a file the installer doesn't create makes Explorer fall back to its
    /// "How do you want to open this file?" chooser, which is exactly what #85 and #52
    /// reported. `${MAINBINARYNAME}` is the only spelling that cannot drift.
    #[test]
    fn installer_hooks_reference_the_binary_through_tauris_own_define() {
        let hooks = include_str!("../installer/hooks.nsh");
        let command_lines: Vec<&str> = hooks
            .lines()
            // NSIS comments start with ';' -- the header comment above deliberately quotes
            // the old broken value to explain the bug, and is not a registry write.
            .filter(|line| !line.trim_start().starts_with(';'))
            .filter(|line| line.contains("$INSTDIR\\"))
            .collect();
        assert!(!command_lines.is_empty(), "no $INSTDIR references found in hooks.nsh");
        for line in command_lines {
            assert!(
                line.contains("$INSTDIR\\${MAINBINARYNAME}.exe"),
                "hooks.nsh must reference $INSTDIR\\${{MAINBINARYNAME}}.exe, not a literal \
                 binary name: {line}"
            );
        }
    }

    #[test]
    fn installer_hooks_register_both_verbs_with_the_quoted_file_argument() {
        let hooks = include_str!("../installer/hooks.nsh");
        for flag in ["--convert", "--compress"] {
            let expected = format!("\"$INSTDIR\\${{MAINBINARYNAME}}.exe\" {flag} \"%1\"");
            assert!(
                hooks.contains(&expected),
                "hooks.nsh is missing the {flag} command string: {expected}"
            );
        }
    }
}
