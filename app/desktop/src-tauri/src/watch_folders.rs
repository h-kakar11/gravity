//! Watch Folders (Phase 4.1): the user designates a folder; any file dropped into it is
//! automatically queued for conversion/compression. Lives entirely in Rust rather than
//! the C++ core -- this is OS-integration work (native filesystem notifications), not job
//! orchestration, and a detected file just triggers the same `createJob` call the
//! frontend already makes through `CoreState`. Uses the `notify` crate (native
//! `ReadDirectoryChangesW` on Windows via `RecommendedWatcher`, not polling) so detection
//! is instant and idle folders cost nothing.
//!
//! Only fires in the background once Phase 4.2's tray/keep-alive mode is active -- the
//! watcher itself doesn't care either way, it "just works" once the process stays alive
//! past the window closing.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use notify::{Event, EventKind, RecommendedWatcher, RecursiveMode, Watcher};
use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter, Manager};

use crate::core_bridge::CoreState;
use crate::paths::gravity_data_dir;

/// Wait this long after the most recent write to a path before treating it as "arrived"
/// -- avoids queuing a file that's still being copied/written.
const DEBOUNCE: Duration = Duration::from_secs(2);

#[derive(Clone, Serialize, Deserialize)]
pub struct WatchFolderConfig {
    pub path: String,
    /// "CONVERSION" | "COMPRESSION" -- DOWNLOAD is deliberately not supported (a watch
    /// folder reacts to local files appearing, which has no meaning for a URL-driven
    /// download job).
    #[serde(rename = "jobType")]
    pub job_type: String,
    #[serde(rename = "defaultOptions")]
    pub default_options: serde_json::Value,
}

struct ActiveWatch {
    // Held only to keep the watcher alive for as long as this entry exists in the map --
    // never read after construction.
    _watcher: RecommendedWatcher,
}

#[derive(Default)]
pub struct WatchFolderState {
    active: Mutex<HashMap<String, ActiveWatch>>,
    pending: Arc<Mutex<HashMap<PathBuf, Instant>>>,
}

fn config_file_path() -> PathBuf {
    gravity_data_dir().join("watch_folders.json")
}

fn load_configs() -> Vec<WatchFolderConfig> {
    match std::fs::read_to_string(config_file_path()) {
        Ok(content) => serde_json::from_str(&content).unwrap_or_default(),
        Err(_) => Vec::new(),
    }
}

fn save_configs(configs: &[WatchFolderConfig]) -> Result<(), String> {
    let path = config_file_path();
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    let json = serde_json::to_string_pretty(configs).map_err(|e| e.to_string())?;
    std::fs::write(&path, json).map_err(|e| e.to_string())
}

/// Restores watchers for every previously-configured folder. Called once from `run()`'s
/// `.setup()`, after `WatchFolderState` has been `app.manage()`d.
pub fn restore_watch_folders(app: &AppHandle) {
    for config in load_configs() {
        let path = config.path.clone();
        if let Err(e) = start_watch(app, config) {
            log::warn!("failed to restore watch folder '{path}': {e}");
        }
    }
}

fn start_watch(app: &AppHandle, config: WatchFolderConfig) -> Result<(), String> {
    let state = app.state::<WatchFolderState>();
    let pending = state.pending.clone();
    let app_handle = app.clone();
    let job_type = config.job_type.clone();
    let default_options = config.default_options.clone();

    let mut watcher = notify::recommended_watcher(move |res: notify::Result<Event>| {
        let Ok(event) = res else { return };
        // Both Create and Modify: a file copy often shows up as a Create followed by one
        // or more Modify events as bytes land; either is a legitimate "something is
        // happening here" signal worth (re)scheduling the debounce timer for.
        if !matches!(event.kind, EventKind::Create(_) | EventKind::Modify(_)) {
            return;
        }
        for path in event.paths {
            if path.is_file() {
                schedule_arrival(
                    pending.clone(),
                    app_handle.clone(),
                    path,
                    job_type.clone(),
                    default_options.clone(),
                );
            }
        }
    })
    .map_err(|e| e.to_string())?;

    watcher
        .watch(Path::new(&config.path), RecursiveMode::NonRecursive)
        .map_err(|e| e.to_string())?;

    state
        .active
        .lock()
        .unwrap()
        .insert(config.path.clone(), ActiveWatch { _watcher: watcher });
    Ok(())
}

fn schedule_arrival(
    pending: Arc<Mutex<HashMap<PathBuf, Instant>>>,
    app: AppHandle,
    path: PathBuf,
    job_type: String,
    default_options: serde_json::Value,
) {
    let seen_at = Instant::now();
    pending.lock().unwrap().insert(path.clone(), seen_at);

    tauri::async_runtime::spawn(async move {
        tokio::time::sleep(DEBOUNCE).await;

        // Only proceed if no newer event arrived for this path while sleeping -- that
        // would mean the file is still being written and a later timer will pick it up.
        let still_current = pending.lock().unwrap().get(&path).copied() == Some(seen_at);
        if !still_current || !path.is_file() {
            return;
        }
        pending.lock().unwrap().remove(&path);

        enqueue_job(&app, &path, &job_type, &default_options).await;
    });
}

async fn enqueue_job(app: &AppHandle, path: &Path, job_type: &str, default_options: &serde_json::Value) {
    let Some(state) = app.try_state::<CoreState>() else {
        return;
    };

    let mut params = if default_options.is_object() {
        default_options.clone()
    } else {
        serde_json::json!({})
    };
    let params_obj = params.as_object_mut().expect("just ensured this is an object");
    params_obj.insert("inputPath".to_string(), serde_json::json!(path.to_string_lossy()));
    if !params_obj.contains_key("outputDirectory") {
        if let Some(parent) = path.parent() {
            params_obj.insert(
                "outputDirectory".to_string(),
                serde_json::json!(parent.to_string_lossy()),
            );
        }
    }

    let create_job_params = serde_json::json!({ "type": job_type, "params": params });
    let (_, rx) = match state.send_request("createJob", create_job_params) {
        Ok(pair) => pair,
        Err(e) => {
            log::warn!("watch folder: failed to submit job for {}: {e}", path.display());
            return;
        }
    };

    match tokio::time::timeout(Duration::from_secs(10), rx).await {
        Ok(Ok(response)) if response.get("ok").and_then(|v| v.as_bool()).unwrap_or(false) => {
            let job_id = response
                .get("result")
                .and_then(|r| r.get("jobId"))
                .and_then(|v| v.as_str())
                .map(str::to_owned);
            let _ = app.emit(
                "watch-folder-triggered",
                serde_json::json!({ "path": path.to_string_lossy(), "jobId": job_id }),
            );
        }
        _ => log::warn!("watch folder: createJob failed or timed out for {}", path.display()),
    }
}

#[tauri::command]
pub fn add_watch_folder(
    app: AppHandle,
    path: String,
    job_type: String,
    default_options: serde_json::Value,
) -> Result<(), String> {
    if job_type != "CONVERSION" && job_type != "COMPRESSION" {
        return Err("watch folders only support the CONVERSION and COMPRESSION job types".to_string());
    }
    if !Path::new(&path).is_dir() {
        return Err(format!("'{path}' is not an existing directory"));
    }

    let config = WatchFolderConfig {
        path: path.clone(),
        job_type,
        default_options,
    };
    start_watch(&app, config.clone())?;

    let mut configs = load_configs();
    configs.retain(|c| c.path != path);
    configs.push(config);
    save_configs(&configs)
}

#[tauri::command]
pub fn remove_watch_folder(app: AppHandle, path: String) -> Result<(), String> {
    app.state::<WatchFolderState>().active.lock().unwrap().remove(&path);

    let mut configs = load_configs();
    configs.retain(|c| c.path != path);
    save_configs(&configs)
}

#[tauri::command]
pub fn list_watch_folders() -> Vec<WatchFolderConfig> {
    load_configs()
}
