// Kept a thin shim per Tauri v2 convention -- real setup lives in lib.rs::run().
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    mediatool_desktop_lib::run();
}
