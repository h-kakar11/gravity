# Phase 6 — Application Shell, Design System & Polish

Phase 5 delivered a working, tested backend and a functional queue UI. It looked like what
it was: a developer console with a job list bolted on — four plain-text tabs, black-on-white
inline styles, unicode arrows for controls, no home screen, no settings screen reachable
from the product, and a raw-JSON debugging page sitting in primary navigation next to
"Download" and "Queue."

Phase 6 is not a backend phase. No queue, scheduler, retry, persistence, or IPC command
changed. This document covers what did: turning that functional UI into Gravity — a single,
deliberately designed, dark, technical desktop application.

## 1. Audit — what was actually there

Read before writing anything: `README.md`, `docs/architecture.md`, `docs/roadmap.md`,
`docs/decisions.md`, `docs/development.md`, `docs/ipc-contract.md`, `docs/phase-2.md`,
`docs/phase-5.md`. Then the entire frontend, file by file (`App.tsx`, every page, every
component, `coreClient.ts`, every type file, `queueReducer.ts`, `useQueue.ts`, `useJobs.ts`,
`jobDisplay.ts`).

Findings, categorized against the spec's own audit checklist:

| Category | Finding |
|---|---|
| Duplicated state | `DownloaderPage` and `DevConsole` each ran their own `useJobs()` polling hook, independent of the `useQueue()` store `QueuePage` used. Two reconciliation paths for the same backend truth. |
| Duplicate components | Three separate inline `styles: Record<string, CSSProperties>` objects (`DownloaderPage.tsx`, `DevConsole.tsx`, `queueStyles.ts`) each redefined roughly the same button/card/banner look with slightly different values — no shared source. |
| Inconsistent spacing/typography | Every page picked its own `padding`, `fontSize`, `borderRadius` literals ad hoc; no scale, so `0.35rem` and `0.4rem` gaps sat next to each other for no reason. |
| Inconsistent button styles | `<button>` with raw inline styles in `DownloaderPage`/`DevConsole`; a `styles.button`/`buttonPrimary`/`buttonDanger` trio in the queue screens with different padding than the other two pages. |
| Inconsistent iconography | Unicode glyphs (`↑ ↓ ⤒ ✕`) as job-row controls; no icon anywhere else in the app. Not one coherent system. |
| Missing states | No dedicated home/dashboard; download workflow had no distinct playlist/no-formats/invalid-URL states (all just "inspect error" text); no toasts; queue empty states existed but with no differentiated "no jobs" vs. "filter matches nothing." |
| Poor navigation hierarchy | `App.tsx`'s four tabs were `Download / Convert & Compress / Queue / Dev Console` — a diagnostics screen with equal billing to the product's actual workflows, and no Home or Settings. |
| Light-mode only | Every color was a literal hex value tuned for a white background (`#fff`, `#f8fafc`, `#eff6ff`) — the opposite of the "dark, premium, technical" identity the spec asks for. |
| Dead code risk | `hooks/useJobs.ts` and `components/queueStyles.ts` became fully dead once the above were fixed — removed rather than left to rot (see §14). |
| IPC handling | `updateSettings` existed in `types/ipc.ts` and was implemented on the backend (`HandleUpdateSettings` in `main.cpp`) but had no `coreClient.ts` export and no UI at all — Settings was unreachable. |

Everything below addresses one or more of these directly.

## 2. Design system

`app/frontend/src/styles/theme.css` — one token set, defined once, consumed everywhere by
class name. No component defines its own color, spacing, radius, or shadow value; if a
value is needed twice it graduates into a token.

- **Surfaces**: a four-step dark scale (`--surface-0` app background through `--surface-3`
  raised/hovered), plus a `--surface-selected` for the active job row.
- **Typography**: one font stack, a six-step type scale (`--text-xs` through `--text-xl`),
  two line-height tokens.
- **Spacing**: a 4px-based scale (`--space-1`…`--space-12`).
- **Radii**: `--radius-sm/md/lg/pill`.
- **Status colors**: one token per job state (`--status-queued` … `--status-skipped`),
  always paired with an icon and the state's text label — never the only signal (spec
  section 3; enforced by `StatusBadge`, the one place a state renders as a badge).
- **Motion**: three duration tokens and one easing curve, wrapped in
  `@media (prefers-reduced-motion: reduce)` that collapses every animation/transition to
  near-zero duration.
- **Focus**: one `--focus-ring` token, applied only via `:focus-visible` — a mouse click
  never rings a button, keyboard navigation always does.

`app/frontend/src/styles/components.css` builds class-based components on those tokens:
`.gv-btn` (four variants below), `.gv-status`, `.gv-card`/`.gv-panel`, `.gv-toolbar`,
`.gv-tabs`, `.gv-row`/`.gv-list` (the job list), `.gv-detail` (the job detail panel),
`.gv-banner` (four tones), `.gv-empty`, `.gv-skeleton`, `.gv-toast-stack`. Nothing in the
rewritten pages defines its own ad hoc inline style beyond a handful of one-off layout
tweaks (`display: grid`, `gridTemplateColumns`) that don't warrant a class.

**Button variants** (spec section 3's exact list): `.gv-btn--primary` (filled accent),
`.gv-btn--secondary` (the default, bordered surface), `.gv-btn--destructive` (outlined red,
used only for Cancel), `.gv-btn--ghost` (borderless, low emphasis — Dismiss, Close). A
disabled button (`:disabled`) drops to 42% opacity and `cursor: not-allowed` app-wide,
implemented once. A shared `<Button>` React component (`components/ui/Button.tsx`) wraps
the class names so a call site writes `variant="primary"` rather than a class string.

**Status variants**: `QUEUED / WAITING / STARTING / RUNNING / PAUSED / RETRY_WAIT /
RETRYING / COMPLETED / FAILED / CANCELLED / SKIPPED` each map to one color token and one
icon in `components/ui/StatusBadge.tsx` — the single place a `JobState` becomes UI, so the
queue list, the detail panel, and the home page's recent-activity strip cannot disagree
about what a given state looks like.

## 3. Icon system

`components/icons.tsx` — 22 hand-authored 20×20 stroke icons (Home, Download, Convert,
Compress, Queue, Settings, Play, Pause, Retry, Cancel, Trash, three chevrons, Check, Alert,
Info, X, Folder, Link, Search, Inbox, a spinner) in one visual language: 1.6px stroke,
rounded caps/joins, `currentColor`. Every icon is `aria-hidden="true"` by default — the
accessible name for an icon-only control (the job row's move/retry/cancel/remove buttons)
comes from its own `aria-label`, never inferred from the glyph. This replaces the unicode
arrows entirely.

## 4. Application shell

`components/AppShell.tsx` — a persistent sidebar (`Home / Download / Convert & Compress /
Queue / Settings`, spec section 4's exact list, nothing added to fill space) with a single
`aria-current="page"` marker for the active screen. Every nav item is a plain `<button>` in
normal DOM order, so keyboard `Tab`/`Shift+Tab` and screen readers work with zero custom
roving-tabindex logic. A skip-link (`Skip to main content`, visually hidden until focused)
precedes the sidebar for keyboard users. Below 860px width the sidebar collapses to a
horizontal bar (verified in the CSS, not click-tested — see §13 "Known limitations").

`App.tsx` holds one `route` state value and one `useQueue()` instance, passed to whichever
page needs it. This is also where the Phase 5→6 state-duplication fix lives: `DownloaderPage`
and `DevConsole` now read the same store `QueuePage` does, instead of each opening a second,
independent subscription (§14, and `docs/decisions.md`).

## 5. Home / Dashboard

`pages/HomePage.tsx` — three launch cards (Download / Convert / Compress) and a compact
queue overview: four stat tiles (Active/Queued/Completed/Failed, reusing the Queue page's
own `QueueStatistics`) and up to five recent running/completed/failed jobs, each a
`StatusBadge` plus title, clicking through to the Queue page. It does not reimplement
filtering, sorting, or per-job controls — those exist in exactly one place (spec section 5:
"do not duplicate the entire queue page").

## 6-7. Download workflow & inspection

`pages/DownloaderPage.tsx` was rewritten with explicit states rather than one blob of
conditional JSX:

- **idle** → typing, no request made yet
- **inspecting** → request in flight, Inspect button shows a spinner
- **ready** → metadata returned; further split into a normal single item, a **playlist**
  (`playlistCount > 1`, shown as an info banner explaining Gravity downloads the first
  linked item, not the whole playlist), and **no formats** (a warning banner, not silently
  showing a broken quality picker)
- **error** → inspection failed; the banner shows the mapped, human message
  (`describeError`) with the backend's own `details` behind a collapsed `<details>`, never a
  raw error code as the primary text
- URLs are validated client-side (`new URL(...)`, requiring `http(s)`) before Inspect is
  even enabled, so "that doesn't look like a web address yet" appears before a round trip,
  not after

Metadata display was trimmed to what a person decides on: title, uploader, duration, format
count, thumbnail. The full yt-dlp format list stays available to the backend (it drives the
quality picker) without being dumped in the UI.

## 8. Processing page

`pages/ProcessPage.tsx` — the Convert/Compress/pipeline mode switch is now icon-labeled
tabs with a one-line description under the tab strip. Compression presets are translated to
plain language rather than raw levels: **"Smaller file"** (most compression), **"Balanced"**
(the middle ground), **"High quality"** (least compression) — each with a one-line
trade-off hint, wired to the same `LOW/MEDIUM/HIGH` wire values the backend already uses
(`docs/ipc-contract.md` unchanged). No FFmpeg flag, codec name, or CRF value appears
anywhere in this page.

## 9-11. Queue, queue states, job detail

`pages/QueuePage.tsx`, `components/JobRow.tsx`, `components/JobDetailPanel.tsx` were
restyled onto the design system without changing their data flow — `queueReducer.ts` (the
Phase 5 reconciliation logic: sequence numbers, per-job revisions, snapshot vs. incremental
events) is untouched.

`JobRow` is now wrapped in `React.memo` (spec section 20, frontend performance): the queue
can produce several `jobProgress` events a second for whichever job is running, and without
memoization every row in the list re-renders on each one because the parent's `jobs` array
is a fresh reference every dispatch. `QueuePage` also only passes a live `nowMs` clock value
to whichever row is actually in `RETRY_WAIT` (the only state whose countdown depends on the
clock) — every other row gets a constant, so the once-a-second retry-countdown tick doesn't
itself defeat the memoization for the rest of the list.

Every state named in spec section 10 was rendered and inspected, not just coded and assumed
correct — see §13 for how ("Long filename", "99% progress", "Unknown ETA" etc. are properties
of `jobDisplay.ts`'s existing formatters, which Phase 5's 30 `jobDisplay.test.ts` cases
already cover at the unit level; Phase 6 added rendering-level tests for the states that are
about component behavior rather than formatting — see §12).

`JobDetailPanel` gained a `StatusBadge` next to its title and an icon on the error block; it
still shows exactly the same fields it did in Phase 5 (job id, type, priority, timestamps,
progress, retries, dependencies, error) and still never shows an FFmpeg argv or a yt-dlp
command line (spec section 21, unchanged).

## 12. Settings

`pages/SettingsPage.tsx` is new. Deliberately small — see `docs/decisions.md` "Settings
scope" for the full audit of which `Settings.h` fields the backend actually acts on versus
which only round-trip to disk. Three editable surfaces:

1. **Show notifications** (`general.showNotifications`) — toggles immediately, gates
   `useQueueNotifications` in `App.tsx`. The only setting with a real, immediate frontend
   effect.
2. **FFmpeg path** (`advanced.ffmpegPath`) — a real backend effect, but only from the next
   launch (the engine is built once at `AppContext` construction); the copy says so rather
   than implying it's live.
3. **Queue concurrency** — shown read-only, pointing at the Queue screen's existing live
   control, rather than as a second control for the same setting that would not itself take
   effect until restart.

A **Developer** section links to the moved `DevConsole` (§4). Every other field in
`Settings.h` is not shown, per spec section 22 ("do not add settings for functionality that
does not exist").

## 13. Toasts

`components/ui/ToastProvider.tsx` (a small React context + auto-dismissing stack, bottom
right, capped at 4 visible) and `state/useQueueNotifications.ts` (the logic: diffs
`queueReducer`'s job map on every state change and fires for a real transition into
`COMPLETED`/`FAILED`/`CANCELLED`/`RETRY_WAIT`). Two things it deliberately does not do:

- **Never fires on progress.** `jobProgress` events never change `JobSnapshot.state`, and
  the diff only looks at `state`, so a job going from 10% to 55% produces zero toasts —
  verified in `useQueueNotifications.test.tsx`.
- **Never floods on connect.** The first loaded snapshot is recorded as a baseline without
  toasting for every job already in it; only a transition witnessed *after* that baseline is
  shown. The one exception is a one-time "Queue restored" toast when the first snapshot
  contains a job whose error is `E_JOB_INTERRUPTED` — the actual signal Phase 5's restart
  recovery leaves behind (`QueuePersistence::ApplyRestartRecovery`), which is the real,
  verifiable meaning of "the queue was restored after a restart," rather than a guess.

## 14. Motion

CSS-only, via the design system's duration/easing tokens: rows fade/slide in on mount
(`.gv-row`, `.gv-detail`), toasts slide in and out, an indeterminate progress bar sweeps
left-to-right when a job's percentage is unknown rather than sitting frozen, a job's
progress-bar width transitions instead of jumping. All of it collapses under
`prefers-reduced-motion: reduce` (theme.css, one global rule). Nothing animates on every
progress tick — the percentage-fill width transition is the only per-progress-event visual
change, and it's a 180ms width interpolation, not a re-triggered animation.

## 15. Dead code removed

- `hooks/useJobs.ts` — the second, duplicate polling hook (§4, §1).
- `components/queueStyles.ts` — superseded by the design system; nothing imports it once
  every page that did was rewritten.

Both were confirmed unreferenced (`grep -rln`) before deletion, and the build/typecheck was
re-run clean afterward.

## 16. Terminology

Standardized on the spec's own vocabulary: **Download / Convert / Compress / Queue /
Retry / Cancel / Completed / Failed**. "Job" is the one word used for a unit of work
throughout the UI and code (`JobSnapshot`, `JobRow`, `jobTitle`) — "task," "process," and
"operation" do not appear as synonyms for it in user-facing text. The exception is
`jobSubtitle`'s per-type "operation" line (e.g. "MP4 → WEBM"), which describes *what a job
does*, not a rename of the word "job" itself.

## 17. Accessibility

- Every icon-only control (job row move/retry/cancel/remove, toast dismiss, panel close)
  has an explicit `aria-label` naming the action and the job it applies to.
- A disabled control keeps its label and gets a `title` explaining *why* it's disabled
  (unchanged from Phase 5, carried into the new component styling).
- Focus is visible everywhere via `:focus-visible` (keyboard only), using one consistent
  ring token.
- The job row is a real `role="button"` with `tabIndex={0}` and `Enter`/`Space` handling
  (unchanged from Phase 5).
- Status is never color-only: `StatusBadge` always pairs a color with an icon and the
  state's text label.
- A skip-link lets a keyboard user bypass the sidebar.
- Toasts use `role="status"` with `aria-live="polite"`, and the toast region itself is
  `role="region"` with a label, so a screen reader announces new toasts without needing to
  be focused on them.

Not done: a full screen-reader pass with an actual assistive-technology tool (not available
in this environment — see §19 "Known limitations"). The above was verified structurally
(correct ARIA attributes present, correct roles, keyboard flow reachable) via the rendering
tests and the real app launch, not by listening to it with a screen reader.

## 18. Frontend performance

- `JobRow` wrapped in `React.memo`; `QueuePage` only threads a changing prop (`nowMs`) into
  rows that actually use it (§9).
- `queueReducer.ts`'s existing sequence/revision-based stale-event rejection (Phase 5,
  unchanged) already prevents redundant state updates from out-of-order or duplicate
  events, which is the other half of "don't rerender for no reason" — Phase 6 didn't touch
  it because it was already doing this correctly.
- No new component subscribes to the event stream directly; everything reads derived state
  from the one `useQueue()` instance, so there's exactly one place progress events cause a
  React state update.

Not measured with dozens of concurrent jobs against real load (the E2E harnesses run a
handful of jobs at a time) — the above is an architectural claim, verified by code
inspection and the memoization unit test, not a profiled benchmark.

## 19. Tests

18 new frontend tests, all `vitest` + newly-added `@testing-library/react` /
`@testing-library/user-event` / `jsdom` (`docs/decisions.md` "RTL added... narrowly"):

| File | Tests | Covers |
|---|---|---|
| `components/AppShell.test.tsx` | 5 | exactly one `aria-current`, `onNavigate` fires with the right route, the five real destinations and nothing else, active-job badge, keyboard reachability |
| `pages/QueuePage.test.tsx` | 7 | connecting state, true-empty vs. filtered-empty (with the way back), action-error banner + dismiss, Cancel sends the right command, Retry disabled on a non-failed job, Pause sends the right command |
| `state/useQueueNotifications.test.tsx` | 6 | no toast for pre-existing jobs on first load, toast on a real COMPLETED/FAILED/CANCELLED transition, no toast on a progress-only change, no toast at all when disabled, restart-recovery toast fires once |

Existing Phase 5 tests (`queueReducer.test.ts` 27, `jobDisplay.test.ts` 30) needed zero
changes — the restyle didn't touch the logic they cover.

**Before Phase 6:** 57 frontend tests. **After:** 75.

## 20. Real UI verification

Screen automation was available in this environment (an X virtual framebuffer, `xdotool`,
`ImageMagick`) but not pre-installed — the C++ build's E2E harnesses (`tests/e2e/`) don't
need a display, so Phase 5 never needed one. Phase 6 does, since the actual claim being
verified is what a person sees, so the tools were installed and the real app was launched:

1. Built the Rust shell for real: `cargo build --manifest-path app/desktop/src-tauri/Cargo.toml`
   (compiles clean, matching the Phase 5 baseline).
2. Started `Xvfb :99 -screen 0 1280x800x24`.
3. Ran the actual `mediatool-core` binary (the same one `ctest`/E2E exercise) and the actual
   `mediatool-desktop` Tauri binary against it, via `MEDIATOOL_CORE_PATH` — the same
   sidecar-resolution mechanism a real launch uses (`core_bridge.rs`).
4. Screenshotted every screen with `import -window root` and read each PNG back: **Home**,
   **Download**, **Convert & Compress**, **Queue**, **Settings**, **Developer console**
   (reached via Settings, confirming it's off primary nav), a **running self-test job**
   (created for real through the IPC self-test path, no simulated progress), and the
   **completed job's detail panel** opened by a real click.

This caught a real bug before it shipped: the first render came back in **light mode**,
because Xvfb reports no `prefers-color-scheme: dark` by default — the same as most
out-of-the-box desktop installs — and the theme's light-mode media-query override activated.
Fixed by making dark the unconditional default (§2, `docs/decisions.md`). The
before/after difference is visible only by actually rendering the app; `tsc`/`vitest`/`vite
build` all passed the whole time, since none of them execute a browser's media-query
resolution.

Also confirmed live via the real app, not just by reading the code: the sidebar's active-job
count badge updated from 0→1 the instant a job started; the Queue page's stat tiles and job
row updated the instant the job completed; `getSettings`/`getHardwareInfo` round-tripped
real data from the real core process (actual CPU name/core count) into the Developer
console's JSON panels; and the job detail panel opened with real timestamps and a correctly
disabled priority selector on a completed job.

## Regression (full suite, after Phase 6)

| Suite | Result |
|---|---|
| C++ (`ctest`, unchanged from Phase 5) | 346 tests, 331 pass, 15 skipped (Windows-only), 0 fail |
| Python (`unittest`, untouched) | 24 pass |
| Frontend (`vitest`) | 75 pass (57 → 75) |
| `tsc --noEmit` | clean |
| `vite build` | clean, 17.6 kB CSS / 221 kB JS (66 kB gzipped) |
| `cargo build` (Tauri shell) | clean |
| Real Tauri app launch | 8 screens rendered and screenshotted (§20) |

## Known limitations

- **No live-mouse click-through video was captured**, only static screenshots per screen —
  the environment has no video capture tool. Every screen was individually driven with real
  clicks (`xdotool`) and a real backend, not merely rendered once with mock data.
- **Windows-specific rendering (high-DPI, OS scaling) was not verified** — this session's
  only display is a Linux virtual framebuffer. The CSS uses relative units throughout
  (`rem`, `%`, `minmax()`/`auto-fit` grids) rather than fixed pixel layouts, which is the
  correct *approach* for DPI tolerance, but the claim of it working on a real Windows
  scaling setting is unverified.
- **No assistive-technology (screen reader) pass** — verified structurally (ARIA
  attributes, roles, keyboard reachability), not by listening to it.
- **Small-window behavior below 860px is CSS-only, not click-tested** — the sidebar's
  responsive collapse rule was written and is present in `components.css`, but this
  session's virtual display was tested only at the app's default 1280×800.
- **Most of `Settings.h`'s fields have no editable UI**, because they have no backend
  effect yet (`docs/decisions.md` "Settings scope"). This is a deliberate scope limit, not
  an oversight — see `docs/roadmap.md` "UI" for what unblocks each one.

## Deliberate deviations

1. **Dark theme is unconditional, not `prefers-color-scheme`-driven** (§2, and
   `docs/decisions.md`) — the opposite of what a generic "theme-aware" guideline would
   suggest, chosen because the product identity explicitly wants dark as the default
   experience, not as one of two equally-weighted options.
2. **Settings page shows three fields out of roughly twenty** in the underlying type,
   because that's how many the backend currently acts on (`docs/decisions.md` "Settings
   scope"). The spec asked for exactly this restraint by name.
3. **`DevConsole` was kept, not deleted**, but moved off primary navigation into Settings →
   Developer — it remains genuinely useful for IPC diagnostics and was built for exactly
   that in Phase 1; removing it would lose real debugging value the spec doesn't ask to lose.

Phase 7 was not started.
