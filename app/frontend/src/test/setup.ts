import "@testing-library/jest-dom/vitest";
import { cleanup } from "@testing-library/react";
import { afterEach } from "vitest";

// vitest's `globals` option is off (see vite.config.ts) so React Testing Library's own
// auto-cleanup, which relies on a global `afterEach`, never registers. Do it explicitly:
// without this, one test's rendered DOM stays mounted into the next, and multiple tests in
// the same file that render the same component collide as "multiple elements found".
afterEach(() => {
  cleanup();
});
