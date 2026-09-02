import { afterEach } from "vitest";
import { cleanup } from "@testing-library/react";

// @testing-library/react's automatic post-test DOM cleanup relies on detecting a global
// `afterEach` -- since this project deliberately imports `afterEach`/`describe`/`it`
// explicitly from "vitest" in every test file (rather than turning on vitest's `globals`,
// which would need extra tsconfig "types" wiring to typecheck under `tsc --noEmit`),
// that auto-detection never fires. Registering it here once, via vitest's `setupFiles`,
// gets the same effect without needing `globals: true`.
afterEach(() => {
  cleanup();
});

// A minimal stand-in for the Tauri webview's injected globals.
//
// Anything rendering a GlassCard mounts useTauriDragDrop, which calls
// `getCurrentWebview()` -- and @tauri-apps/api reads `window.__TAURI_INTERNALS__.metadata`
// unguarded, so under jsdom that throws inside a passive effect and takes the render down.
// Real Tauri always injects these before any app code runs; jsdom does not, which made
// every page built out of GlassCards untestable. This restores the one assumption the
// bridge is entitled to make, and nothing more: no command is answered here, so tests
// still mock `services/coreClient` for anything they actually rely on.
type TauriInternals = {
  metadata: { currentWindow: { label: string }; currentWebview: { label: string; windowLabel: string } };
  transformCallback: (callback: (...args: unknown[]) => void, once?: boolean) => number;
  unregisterCallback: (id: number) => void;
  invoke: (cmd: string) => Promise<unknown>;
};

const callbacks = new Map<number, (...args: unknown[]) => void>();
let nextCallbackId = 1;

(globalThis as unknown as { window: { __TAURI_INTERNALS__: TauriInternals } }).window.__TAURI_INTERNALS__ = {
  metadata: {
    currentWindow: { label: "main" },
    currentWebview: { label: "main", windowLabel: "main" },
  },
  transformCallback(callback, once = false) {
    const id = nextCallbackId++;
    callbacks.set(id, (...args) => {
      if (once) callbacks.delete(id);
      callback(...args);
    });
    return id;
  },
  unregisterCallback(id) {
    callbacks.delete(id);
  },
  // Never resolves to anything meaningful on purpose: a test that depends on a real
  // command's result should mock services/coreClient, not lean on this.
  invoke: () => Promise.resolve(undefined),
};

// The event plugin's own injected global, read by @tauri-apps/api's unlisten path. Without
// it, unsubscribing on unmount throws during React's passive-effect cleanup.
(globalThis as unknown as {
  window: { __TAURI_EVENT_PLUGIN_INTERNALS__: { unregisterListener: (event: string, id: number) => void } };
}).window.__TAURI_EVENT_PLUGIN_INTERNALS__ = {
  unregisterListener: () => {},
};
