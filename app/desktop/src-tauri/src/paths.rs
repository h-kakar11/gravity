//! Shared "%LOCALAPPDATA%\Gravity\" resolution for the small Rust-owned JSON config
//! files (watch_folders.json, scheduled_tasks.json) that live alongside the C++ core's
//! own settings.json/job_history.json/presets.json in the same directory -- see
//! core/settings/JsonFileSettingsStore.h for why this location was chosen (consistent
//! with the rest of the app, no round-trip through the core process needed since this
//! state is Rust-local).

use std::path::PathBuf;

pub fn gravity_data_dir() -> PathBuf {
    match std::env::var("LOCALAPPDATA") {
        Ok(local_app_data) => PathBuf::from(local_app_data).join("Gravity"),
        Err(_) => PathBuf::from("Gravity"),
    }
}
