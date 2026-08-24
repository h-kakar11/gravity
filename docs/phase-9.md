# Phase 9 — V1 Completion & Feature Freeze

Phases 1-8 built a working, packaged, hardened application. Phase 9's job was narrower:
read the whole roadmap against what's actually built, find the genuine gaps between "a
capable media tool" and "a v1 someone could reasonably call finished," close only those
gaps, and then stop. This is not a feature-expansion phase — the definition of done here is
a frozen scope, not a longer feature list.

## 1. Product audit

### MUST HAVE for v1 (and their status)

| Capability | Status |
|---|---|
| Download a single video/audio URL, choose quality, watch progress, get a verified file | **Working**, real end-to-end (Phase 2, re-verified Phase 8) |
| Convert a local file between supported formats | **Working**, real FFmpeg (Phase 5) |
| Compress a local file with understandable presets | **Working**, real FFmpeg (Phase 5) |
| Download → convert → compress as one pipeline | **Working**, dependency model (Phase 5) |
| A single, coherent queue: concurrency, priority, reorder, pause/resume, cancel, retry | **Working** (Phase 5) |
| Survive a restart without losing or corrupting queue state | **Working**, real crash-recovery testing (Phase 5, re-verified Phase 8) |
| Look and feel like one deliberately designed application | **Working** (Phase 6) |
| Install and run without a development environment | **Working in principle; not fully verified** — no Windows machine in this environment to build/test the actual installer (Phase 7's documented limitation, carried forward) |
| Basic product information (version, dependency versions) | **Working** (this phase, §2 below) |
| Errors that explain what happened and what to do | **Working** (Phase 6's error-UX pass, re-confirmed this phase, §5) |

Every MUST HAVE was already real before this phase except the last two, which this phase
closed.

### SHOULD HAVE (valuable, correctly deferred for v1)

- **Playlist support.** Genuinely useful, and the architecture (`IDownloadProvider`,
  per-item job creation via the existing dependency-free `createJob` path) supports it
  without a redesign. Deferred anyway: implementing it properly — real playlist
  inspection, item selection UI, duplicate detection across a batch, "don't blindly enqueue
  hundreds of jobs" awareness the spec itself asks for — is a multi-day feature in its own
  right, not a gap-fill. Gravity's core journey (a single URL to a verified file) is
  complete and solid without it; a user who wants a playlist today queues its videos one at
  a time, which works. See `docs/roadmap.md` for what a real implementation would need.
- **A native file/folder picker.** Currently a plain text field for paths — functional, not
  polished. Deferred deliberately, not from neglect: swapping in Tauri's native dialog is a
  contained change, but every path field in the app (download destination, conversion
  input/output, compression input/output) would need it simultaneously to avoid an
  inconsistent product, and none of it changes whether the underlying operations work
  correctly. A cosmetic gap, not a v1 blocker.

### NICE TO HAVE / NOT v1 (deferred, with reasons already on record)

- **GIF two-pass palette generation** — the spec is explicit that a poor one-pass pipeline
  should not be faked in as GIF support. Not implemented; GIF is not in the current target
  format set (`core/media/ProcessingOptions.h`), and stays that way for v1.
- **Hardware encoding (NVENC/QSV/AMF)** — `core/hardware/HardwareInfo.h`'s own header
  comment has documented this as a deferred Phase 2 TODO since Phase 1; detection exists
  (`availableEncoders`, currently always empty by design), the encoder path does not. The
  spec's own bar for implementing this — reliable detection, correct fallback, coherent
  presets, testability, reliable packaging — is not something this environment (no GPU, no
  Windows machine) could respectably build or verify, so it stays deferred rather than
  built untested.
- **Target-size compression ("~500MB")** — would need real iterative/two-pass bitrate
  calculation to mean anything; a CRF preset pretending to hit a target size would be
  actively misleading. Not implemented. The existing quality-preset compression (spec
  section 9's honest alternative) is what ships.

### Explicitly out of scope, always (unchanged since Phase 1)

No accounts, no analytics/telemetry, no cloud processing or uploads — see
`docs/architecture.md` and `docs/decisions.md`.

## 2. About / Help (implemented this phase)

Settings gained an About section: Gravity's own version, FFmpeg's version, and yt-dlp's
version, each genuinely queried (a new `getVersionInfo` IPC command; see the commit for
the full mechanism) rather than hardcoded, and each one shown as absent — not a blank or a
fabricated value — when that dependency isn't available. Verified against the real binary,
real FFmpeg, and the real running Tauri app (screenshot in the commit's own verification,
not repeated here). No developer debugging information is exposed by this panel — it shows
exactly the three version strings, nothing else.

## 3. Core user journeys — re-verified, not re-built

All six journeys the spec names were already real, working, and end-to-end tested before
this phase (Phase 2 for downloads, Phase 5 for conversion/compression/pipelines/queue
control, Phase 5's persistence work for restart recovery). This phase re-ran the evidence
rather than re-implementing anything:

| Journey | Evidence |
|---|---|
| A: URL → inspect → quality → download → queue → progress → completion → locate output | `tests/e2e/queue_download_e2e.py` §1 (real binary, real subprocess, ffprobe-verified output); `DownloaderPage.tsx` (Phase 6 UI, verified via real Tauri launch) |
| B: local file → convert → format/preset → queue → process → verify | `tests/e2e/queue_ffmpeg_e2e.py` (real ffmpeg); `ProcessPage.tsx` |
| C: local file → compress → preset → queue → process → verify | same harness, compression jobs |
| D: download → convert → compress as a pipeline | `queue_download_e2e.py` §6 and `queue_ffmpeg_e2e.py`'s dependency sections — real jobs, real dependency resolution, no path guessing |
| E: create several jobs → reorder → prioritize → pause → resume → cancel → retry | `SchedulerCoreStressTest.cpp` (Phase 8, 5000 operations) plus the E2E suites' retry/cancel/pause sections; `QueuePage.tsx` |
| F: close → reopen → recover queue | `queue_ffmpeg_e2e.py` §13 (real restart, real recovery); re-confirmed this phase's regression run |

All were re-run as part of this phase's final regression (§6) and are green.

## 4. Download quality UX, file handling, format coverage — reviewed, no changes needed

- **Quality presets** (`BEST`/`2160P`/.../`AUDIO_ONLY`) remain the only exposed vocabulary;
  no raw yt-dlp format string is ever shown in the UI (`docs/decisions.md`).
- **File handling**: reviewed against "evaluate whether native pickers are now
  appropriate" — see §1's SHOULD HAVE entry. Deliberately not added this phase.
- **Format coverage**: `core/media/ProcessingOptions.h` remains the curated, closed set
  (MP4/MKV/WEBM/MOV video; MP3/WAV/M4A/FLAC/OPUS audio) — documented in `README.md`'s status
  table, not claimed as "anything FFmpeg supports."

## 5. Error experience — audited against the spec's example list

| Failure | Message a user sees (from `utils/jobDisplay.ts`'s `ERROR_MESSAGES`, Phase 6) |
|---|---|
| Invalid URL | "That is not a supported media URL." |
| Missing/unavailable source | "The input file could not be found. It may have been moved or deleted." |
| Network failure | Mapped to the download's own transient-failure classification, with automatic retry (Phase 5) |
| Permission failure | Backend `PermissionError` category surfaces as a specific, actionable message, not a raw errno |
| Disk full | `E_INSUFFICIENT_DISK_SPACE` → "Not enough free space in the output folder." |
| FFmpeg failure | Was previously always missing its diagnostic detail (Phase 8's stderr-capture fix, §16 below); the human-readable message and the "Technical details" disclosure both now work correctly |
| Dependency failure | "Skipped because a job it depended on did not finish." |
| Retry exhausted | The job's own FAILED state with its last real error, plus a retry count shown in the detail panel |

No changes needed this phase beyond the Phase 8 fix that made FFmpeg's `details` field
actually populate — everything else was already in place from Phase 6.

## 6. Settings — reviewed, unchanged

Already deliberately minimal as of Phase 6/7 (`docs/decisions.md` "Settings scope"): only
fields with a demonstrated real effect are editable. This phase added the About section
(§2) but no new editable setting — nothing new crossed into "has a real backend effect"
territory that would justify one.

## 7. Testing — final journey re-run

| Suite | Result |
|---|---|
| C++ (`ctest`) | 362 tests, 347 pass, 15 skipped (Windows-only), 0 fail |
| Python (`unittest`) | 26 pass (2 new this phase, for `--version`) |
| Frontend (`vitest`) | 75 pass, unchanged |
| E2E: real ffmpeg | 77/77 (2 new this phase, for `getVersionInfo`) |
| E2E: real downloads/retries/dependencies | 34/34 |

## Feature freeze

`docs/v1-feature-freeze.md` is the formal freeze document this phase produces. From this
point forward, per the spec's own instruction: only bug fixes, security fixes, performance
fixes, and release blockers. No new major features.

Phase 10 (release) was not started as part of writing this document.
