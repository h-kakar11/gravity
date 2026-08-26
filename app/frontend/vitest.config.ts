import { defineConfig } from "vitest/config";
import react from "@vitejs/plugin-react";

// Separate from vite.config.ts (kept minimal/Tauri-specific) since Vitest's own config type
// isn't quite the same shape as Vite's -- this is the "First frontend tests" seam from
// Phase 3.4: useJobs.ts's event-merging and NavigationContext are the two pieces of real
// logic introduced so far worth covering.
export default defineConfig({
  plugins: [react()],
  test: {
    environment: "jsdom",
    setupFiles: ["src/testSetup.ts"],
  },
});
