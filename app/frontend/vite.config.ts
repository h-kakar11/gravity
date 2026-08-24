import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// Tauri-specific tuning per https://v2.tauri.app/start/frontend/vite/ -- fixed port so the
// Rust shell's devUrl matches, and Tauri's own console output must not be clobbered by HMR.
export default defineConfig({
  plugins: [react()],
  clearScreen: false,
  server: {
    port: 1420,
    strictPort: true,
    watch: {
      ignored: ["**/src-tauri/**"],
    },
  },
  test: {
    environment: "jsdom",
    setupFiles: ["./src/test/setup.ts"],
  },
});
