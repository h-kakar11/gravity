// Kept a thin shim per Tauri v2 convention -- real setup lives in lib.rs::run().
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    gravity_desktop_lib::run();
}
