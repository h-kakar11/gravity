//! Scheduled Tasks (Phase 4.3): user-defined cron schedules that fire a `createJob` call at
//! the configured time -- e.g. "convert everything queued under this preset every night at
//! 2am" -- as opposed to Watch Folders (4.1), which reacts to a file arriving rather than a
//! clock. Lives in Rust for the same reason as 4.1/4.2: it must fire without the window
//! open, riding the same tray-kept-alive process 4.2 establishes. Hard dependency on 4.2 --
//! there is no point building this before the process survives the window closing.
//!
//! Cron expressions use the `cron` crate's 6-field syntax (seconds first: `sec min hour
//! day month day-of-week`), not the 5-field Unix convention -- e.g. "every day at 2am" is
//! `"0 0 2 * * *"`. The frontend's scheduling UI is expected to build this string from a
//! friendlier picker rather than have the user type it directly.

use std::collections::HashMap;
use std::path::PathBuf;
use std::str::FromStr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;
use std::time::Duration;

use chrono::{DateTime, Utc};
use cron::Schedule;
use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter, Manager};

use crate::core_bridge::CoreState;
use crate::paths::gravity_data_dir;

/// How often the background loop wakes to check whether any task is due. Coarser than
/// Watch Folders' debounce is fine -- cron schedules are inherently minute-granularity by
/// convention, so checking more often buys nothing.
const POLL_INTERVAL: Duration = Duration::from_secs(30);

#[derive(Clone, Serialize, Deserialize)]
pub struct ScheduledTaskConfig {
    pub id: String,
    pub name: String,
    #[serde(rename = "cronExpression")]
    pub cron_expression: String,
    /// "CONVERSION" | "COMPRESSION" | "DOWNLOAD" -- unlike Watch Folders, DOWNLOAD is
    /// meaningful here: a scheduled task isn't reacting to a local file, so a saved URL (in
    /// `params`) is a legitimate thing to schedule.
    #[serde(rename = "jobType")]
    pub job_type: String,
    pub params: serde_json::Value,
    pub enabled: bool,
}

#[derive(Default)]
pub struct ScheduledTaskState {
    tasks: Mutex<HashMap<String, ScheduledTaskConfig>>,
    last_checked: Mutex<HashMap<String, DateTime<Utc>>>,
}

fn config_file_path() -> PathBuf {
    gravity_data_dir().join("scheduled_tasks.json")
}

fn load_configs() -> Vec<ScheduledTaskConfig> {
    match std::fs::read_to_string(config_file_path()) {
        Ok(content) => serde_json::from_str(&content).unwrap_or_default(),
        Err(_) => Vec::new(),
    }
}

fn save_configs(configs: &[ScheduledTaskConfig]) -> Result<(), String> {
    let path = config_file_path();
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    let json = serde_json::to_string_pretty(configs).map_err(|e| e.to_string())?;
    std::fs::write(&path, json).map_err(|e| e.to_string())
}

/// Loads persisted tasks into `ScheduledTaskState` and starts the polling loop. Called once
/// from `run()`'s `.setup()`, after `ScheduledTaskState` has been `app.manage()`d -- mirrors
/// `watch_folders::restore_watch_folders`.
pub fn start_scheduler(app: &AppHandle) {
    {
        let state = app.state::<ScheduledTaskState>();
        let mut tasks = state.tasks.lock().unwrap();
        for config in load_configs() {
            tasks.insert(config.id.clone(), config);
        }
    }

    let app_handle = app.clone();
    tauri::async_runtime::spawn(async move {
        let mut interval = tokio::time::interval(POLL_INTERVAL);
        loop {
            interval.tick().await;
            check_due_tasks(&app_handle).await;
        }
    });
}

async fn check_due_tasks(app: &AppHandle) {
    let state = app.state::<ScheduledTaskState>();
    let due: Vec<ScheduledTaskConfig> = {
        let tasks = state.tasks.lock().unwrap();
        let last_checked = state.last_checked.lock().unwrap();
        let now = Utc::now();
        let due: Vec<ScheduledTaskConfig> = tasks
            .values()
            .filter(|t| t.enabled)
            .filter(|t| is_due(t, now, &last_checked))
            .cloned()
            .collect();
        drop(last_checked);

        let mut last_checked = state.last_checked.lock().unwrap();
        for task in &due {
            last_checked.insert(task.id.clone(), now);
        }
        due
    };

    for task in due {
        fire_task(app, &task).await;
    }
}

/// A task is due if its schedule has an occurrence in `(since, now]`, where `since` is the
/// last time this task was checked -- so an occurrence landing between two polls is never
/// missed, but nothing fires twice for the same occurrence.
fn is_due(task: &ScheduledTaskConfig, now: DateTime<Utc>, last_checked: &HashMap<String, DateTime<Utc>>) -> bool {
    let Ok(schedule) = Schedule::from_str(&task.cron_expression) else {
        return false;
    };
    let since = last_checked
        .get(&task.id)
        .copied()
        .unwrap_or_else(|| now - chrono::Duration::from_std(POLL_INTERVAL).unwrap());
    schedule.after(&since).take(1).any(|occurrence| occurrence <= now)
}

async fn fire_task(app: &AppHandle, task: &ScheduledTaskConfig) {
    let Some(state) = app.try_state::<CoreState>() else {
        return;
    };

    let create_job_params = serde_json::json!({ "type": task.job_type, "params": task.params });
    let (_, rx) = match state.send_request("createJob", create_job_params) {
        Ok(pair) => pair,
        Err(e) => {
            log::warn!("scheduled task '{}': failed to submit job: {e}", task.name);
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
                "scheduled-task-fired",
                serde_json::json!({ "taskId": task.id, "taskName": task.name, "jobId": job_id }),
            );
        }
        _ => log::warn!("scheduled task '{}': createJob failed or timed out", task.name),
    }
}

#[tauri::command]
pub fn add_scheduled_task(
    app: AppHandle,
    name: String,
    cron_expression: String,
    job_type: String,
    params: serde_json::Value,
) -> Result<ScheduledTaskConfig, String> {
    if job_type != "CONVERSION" && job_type != "COMPRESSION" && job_type != "DOWNLOAD" {
        return Err("unsupported job type for a scheduled task".to_string());
    }
    Schedule::from_str(&cron_expression).map_err(|e| format!("invalid cron expression: {e}"))?;

    let config = ScheduledTaskConfig {
        id: generate_task_id(),
        name,
        cron_expression,
        job_type,
        params,
        enabled: true,
    };

    let state = app.state::<ScheduledTaskState>();
    state.tasks.lock().unwrap().insert(config.id.clone(), config.clone());
    persist(&state);
    Ok(config)
}

#[tauri::command]
pub fn update_scheduled_task(
    app: AppHandle,
    id: String,
    name: Option<String>,
    cron_expression: Option<String>,
    enabled: Option<bool>,
    params: Option<serde_json::Value>,
) -> Result<ScheduledTaskConfig, String> {
    if let Some(ref expr) = cron_expression {
        Schedule::from_str(expr).map_err(|e| format!("invalid cron expression: {e}"))?;
    }

    let state = app.state::<ScheduledTaskState>();
    let updated = {
        let mut tasks = state.tasks.lock().unwrap();
        let task = tasks
            .get_mut(&id)
            .ok_or_else(|| format!("no scheduled task with id '{id}'"))?;
        if let Some(name) = name {
            task.name = name;
        }
        if let Some(expr) = cron_expression {
            task.cron_expression = expr;
        }
        if let Some(enabled) = enabled {
            task.enabled = enabled;
        }
        if let Some(params) = params {
            task.params = params;
        }
        task.clone()
    };
    persist(&state);
    Ok(updated)
}

#[tauri::command]
pub fn remove_scheduled_task(app: AppHandle, id: String) -> Result<(), String> {
    let state = app.state::<ScheduledTaskState>();
    state.tasks.lock().unwrap().remove(&id);
    state.last_checked.lock().unwrap().remove(&id);
    persist(&state);
    Ok(())
}

#[tauri::command]
pub fn list_scheduled_tasks(app: AppHandle) -> Vec<ScheduledTaskConfig> {
    app.state::<ScheduledTaskState>()
        .tasks
        .lock()
        .unwrap()
        .values()
        .cloned()
        .collect()
}

fn persist(state: &ScheduledTaskState) {
    let configs: Vec<ScheduledTaskConfig> = state.tasks.lock().unwrap().values().cloned().collect();
    if let Err(e) = save_configs(&configs) {
        log::warn!("failed to persist scheduled_tasks.json: {e}");
    }
}

/// pid + timestamp + a process-local counter is unique enough for a single-user desktop
/// app's local config file; not a security boundary, so pulling in the `uuid` crate for
/// this one call isn't worth it.
fn generate_task_id() -> String {
    static COUNTER: AtomicU64 = AtomicU64::new(0);
    let n = COUNTER.fetch_add(1, Ordering::Relaxed);
    format!("{}-{}-{}", std::process::id(), Utc::now().timestamp_nanos_opt().unwrap_or(0), n)
}
