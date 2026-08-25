//! Windows context menu CLI contract (Phase 5.3, locked decision #3):
//! `gravity.exe --convert "<path>"` / `--compress "<path>"` (a bare path with no flag
//! defaults to `--convert`). The registry entries `installer/hooks.nsh` writes launch
//! `Gravity.exe` with exactly one of these forms per right-click.
//!
//! Two delivery paths, matching the plan's IPC surface table:
//! - **Cold start** (Gravity wasn't already running): args are stashed in `CliState` here
//!   in `run()`'s `.setup()`, *before* the frontend has mounted and could `listen()` for an
//!   event -- the frontend instead calls the `get_startup_file_action` command once on
//!   mount to fetch-and-consume whatever was stashed.
//! - **Already running** (`tauri-plugin-single-instance` intercepted a second launch):
//!   the frontend's already mounted and listening, so this emits the `cli-file-opened`
//!   event directly instead.

use std::sync::Mutex;

use serde::Serialize;
use tauri::{AppHandle, Emitter, Manager};

use crate::tray::show_main_window;

const CLI_FILE_OPENED_EVENT: &str = "cli-file-opened";

#[derive(Clone, Serialize)]
pub struct StartupFileAction {
    path: String,
    mode: String,
}

#[derive(Default)]
pub struct CliState {
    pending: Mutex<Option<StartupFileAction>>,
}

/// Parses `--convert <path>` / `--compress <path>` / a bare `<path>` (defaults to
/// `"convert"`) out of an argv-style slice. `args[0]` is assumed to be the exe path itself
/// (true both for `std::env::args()` and for the argv `tauri-plugin-single-instance`'s
/// callback hands back), so parsing starts from index 1. Returns `None` for a normal
/// launch with no file argument at all.
fn parse_cli_args(args: &[String]) -> Option<(String, String)> {
    let mut mode = "convert".to_string();
    let mut path: Option<String> = None;

    let mut iter = args.iter().skip(1);
    while let Some(arg) = iter.next() {
        match arg.as_str() {
            "--convert" => {
                mode = "convert".to_string();
                path = iter.next().cloned();
            }
            "--compress" => {
                mode = "compress".to_string();
                path = iter.next().cloned();
            }
            other => path = Some(other.to_string()),
        }
    }

    path.map(|p| (p, mode))
}

/// Cold-start path: called once from `run()`'s `.setup()`. Stashes the parsed action (if
/// any) for the frontend to pick up via `get_startup_file_action` once it has mounted --
/// emitting an event this early would race the frontend's `listen()` call not being
/// registered yet.
pub fn store_startup_action(app: &AppHandle, args: &[String]) {
    let Some((path, mode)) = parse_cli_args(args) else {
        return;
    };
    if let Some(state) = app.try_state::<CliState>() {
        *state.pending.lock().unwrap() = Some(StartupFileAction { path, mode });
    }
}

#[tauri::command]
pub fn get_startup_file_action(state: tauri::State<CliState>) -> Option<StartupFileAction> {
    state.pending.lock().unwrap().take()
}

/// Already-running path: called from the `tauri_plugin_single_instance::init` callback
/// (registered first in `run()`'s Builder chain) when a second launch's args get handed to
/// this already-running instance. The frontend is already mounted here, so this emits the
/// event directly rather than going through `CliState`. Always brings the window forward,
/// even for a plain second launch with no file argument -- that's still a reasonable
/// "the user tried to open Gravity again" signal.
pub fn handle_second_instance(app: &AppHandle, args: &[String]) {
    show_main_window(app);
    if let Some((path, mode)) = parse_cli_args(args) {
        let _ = app.emit(CLI_FILE_OPENED_EVENT, StartupFileAction { path, mode });
    }
}
