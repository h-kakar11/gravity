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
