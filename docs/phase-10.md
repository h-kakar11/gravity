# Phase 10 — Release

Phase 9 froze v1's scope. Phase 10's job was to actually get a release candidate out the
door: a real clean build, real (not narrated) test counts, a real crash-recovery and
security pass, a real license inventory, one consistent version number, and clean git
history to tag. This document is that record. Per the spec: there is no Phase 11.

## 1. Release-candidate audit

A repo-wide grep for `TODO|FIXME|HACK|DEBUG|TEMP|PLACEHOLDER|NOT_IMPLEMENTED` across real
source (excluding `node_modules`) found exactly what Phase 7/8 already knew about and
nothing new:

- Two legitimate `TODO`s in `core/hardware/HardwareInfo.h` and
  `WindowsHardwareDetector.cpp` — hardware-encoder detection, deliberately deferred since
  Phase 1 and re-confirmed out of scope for v1 in `docs/v1-feature-freeze.md`.
- One `E_NOT_IMPLEMENTED` error code in `FFmpegEngine.cpp` — a real, intentional error for
  an operation the engine deliberately doesn't support, not a stub.
- Ordinary `Log::Debug` calls and code comments that legitimately use the English word
  "debug"/"temp" in context (e.g. "debug/log chatter" describing yt-dlp's own output) — not
  markers of unfinished work.

## 2. Brand audit

A grep for `MediaTool`/`mediatool` outside legitimate uses (the `mediatool` C++ namespace,
`mediatool-core` binary name — never renamed on purpose, since renaming the sidecar binary
itself was never part of the brand — and `MediaToolException`, a class name) found one real,
previously-missed inconsistency: `core/filesystem/TempDirectory.cpp`'s non-Windows dev-
fallback path (used only when `LOCALAPPDATA` is unset) still created a literal
`.mediatool-temp` directory, and three header comments in `TempDirectory.h`, `Logger.h`, and
`JsonFileSettingsStore.h` still described `%LOCALAPPDATA%\MediaTool\...` even though the
`.cpp` files behind them had already been renamed to `Gravity` in Phase 7. Fixed (renamed to
`.gravity-temp`, comments corrected) after confirming no test depended on the literal old
name. Two references to `%LOCALAPPDATA%\MediaTool\` remain in `docs/phase-7.md`, describing
what a *pre-Phase-7* dev build's old data looks like — a deliberate historical reference,
not a stale one, left alone per the spec's own instruction not to blindly replace those.

## 3. Clean build from source

`build/linux-debug` deleted and reconfigured from scratch:

```
cmake -S . -B build/linux-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/linux-debug -j$(nproc)
```

56s, 87 targets, zero errors, zero warnings. `ctest`, the Python suite, the frontend suite
(`vitest`, `tsc --noEmit`, `vite build`), and `cargo build`/`cargo check` for the Tauri shell
all ran clean from this same from-scratch build — see §6 for exact counts.

## 4. Two real bugs found and fixed this phase

Consistent with Phase 8's central lesson — real testing under real failure conditions finds
what mocks and code review don't — this phase's crash-recovery re-verification (§5) found
one genuine, previously-uncovered bug, plus one test-coverage gap that had been silently
relying on the download path's cancellation logic never actually being exercised end to end:

**A real crash could leave a stale scratch file behind forever.** A CONVERSION/COMPRESSION
job that is actively writing when the app is killed abruptly (`SIGKILL`, a real crash, power
loss — not graceful cancellation or shutdown, both already correct) never runs
`AtomicWriter`'s destructor, so its `<name>.processing.<ext>` temp file survives on disk. If
that specific job is later retried, FFmpeg simply overwrites the stale file and nothing is
ever noticed — which is exactly how this went uncaught for nine phases: every existing test
that exercised recovery also happened to retry the recovered job. If the job is never
retried, the file leaks permanently. Reproduced directly against the real binary (`SIGKILL`
mid-compression, inspect the output directory before any restart) before writing the fix.

Fixed in `JobManager::RestoreFromDisk()`: every CONVERSION/COMPRESSION job that restart
recovery marks interrupted now has its output directory swept for stale
`*.processing.*` artifacts immediately, before any retry — scoped tightly (only directories
named by a job just marked interrupted, only files matching `AtomicWriter`'s own exclusive
marker) so a real file that merely shares the directory is never touched. Covered by a new
unit test (`JobManagerQueueTest.RestoreSweepsAStaleProcessingArtifactLeftByACrash`, which
also asserts an unrelated real file survives) and a new real end-to-end test
(`queue_ffmpeg_e2e.py` §15: `SIGKILL` the live core process mid-compression, restart,
confirm the job recovers to FAILED with a real explanation and the scratch file is gone
before any retry).

**Download cancellation had unit-only coverage.** `DownloadJob`'s `CleanupArtifacts()` was
tested against `MockFileSystem`, but no test ever cancelled a *real* download subprocess and
checked the real filesystem — the FFmpeg path had exactly this coverage (`queue_ffmpeg_e2e.py`
§7) and the download path did not. Added `queue_download_e2e.py` §7: cancel a real running
download (a `FAKE_DL_HANG` mode added to the E2E fixture so it writes a real `.part` artifact
and then hangs until killed), confirm it ends CANCELLED with no new files left in the output
directory. Passed on the first real run — no bug found here, but the path was genuinely
unverified before, not merely untested-and-fine-by-assumption.

**A related orphan-process finding, investigated but not fixed.** The same repro that found
the artifact leak also showed that `SIGKILL`ing the core process does not kill its `ffmpeg`
child — it becomes a real orphan that keeps running until it exits on its own, exactly the
scenario spec section 26/Phase 8 worried about for graceful cancellation (already correct)
but not for an abrupt parent kill. A real fix needs OS-level process lifetime binding
(Windows Job Objects with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, or `prctl(PR_SET_PDEATHSIG)`
on Linux) — `reproc`, the process library this project uses, exposes no such option in its
public API. This is Windows-specific, low-level work this environment cannot build or
responsibly verify (no Windows machine — the same limitation Phase 7 already carries for the
installer itself). Documented as a known limitation (§8), not silently accepted: the actual
user-visible consequence is bounded (never the final named output file, only ever a scratch
file, and now swept on the next startup regardless) and the graceful paths (cancel, normal
shutdown) — the ones a real user actually triggers — are already fully correct and tested.

## 5. Crash recovery — re-verified across every state the spec names

| State at kill | Evidence |
|---|---|
| Queued | `queue_ffmpeg_e2e.py` §13 (graceful shutdown, job never started, survives restart with its id/priority intact) |
| Active/running (download) | `JobManagerQueueTest.InterruptedJobsComeBackAsRetryableFailures` (unit, `JobType::Download`) — recovers to `FAILED`/`E_JOB_INTERRUPTED`, never silently re-runs, never reports COMPLETED |
| Active/processing (conversion/compression) | New this phase: real `SIGKILL` of the live binary mid-compression (`queue_ffmpeg_e2e.py` §15) — recovers to `FAILED`, real explanation, stale artifact swept, queue fully usable afterward |
| Retry-wait | `JobManagerQueueTest.RetryStateSurvivesARestart` (unit) — state, attempt count, and `nextRetryAtMs` all survive intact |
| Corrupt state file | `queue_ffmpeg_e2e.py` §14 — starts clean, corrupt file quarantined (`.corrupt-<timestamp>`) for diagnosis, never silently destroyed, queue fully usable afterward |

No impossible or corrupted state was produced in any of the above. `queue.json` was
inspected directly after each scenario in this phase's own manual runs (schema-versioned,
`records`/`pendingOrder`/`runState` all well-formed) in addition to the automated checks
above.

## 6. Full regression (this phase's final numbers)

| Suite | Result |
|---|---|
| C++ (`ctest`, clean rebuild) | 363 tests, 348 pass, 15 skipped (Windows-only), 0 fail |
| Python (`unittest`) | 26 pass |
| Frontend (`vitest`) | 75 pass |
| `tsc --noEmit` | clean |
| `vite build` | clean, 17.3 kB CSS / 223 kB JS (67 kB gzipped) |
| `cargo build` / `cargo check` (Tauri shell) | clean |
| E2E: real ffmpeg (`queue_ffmpeg_e2e.py`) | 83/83 (6 new this phase: real-crash recovery) |
| E2E: real downloads/retries/dependencies (`queue_download_e2e.py`) | 37/37 (3 new this phase: real cancellation) |
| E2E: IPC fuzzing (`ipc_fuzz.py`) | 77/77 |
| Real Tauri app launch (Xvfb) | Home, Download, Convert & Compress, Queue, Settings (incl. About), Developer console, a real running/completed self-test job — all re-screenshotted, no visual defects found |

Process/file leak audit after the full regression: zero leftover `mediatool-core`/`ffmpeg`/
`ffprobe`/`fake_downloader` processes (`ps aux`), zero `.part`/`.processing` files left in
any E2E output directory.

## 7. Security and license review

**Security**: re-confirmed rather than re-run from scratch — Phase 8's IPC fuzzer already
covers malformed/hostile/oversized input, path traversal, and unknown commands (77/77, still
green). This phase's own grep pass found zero `system()`/`popen()`/shell-string-building
calls in product code (only a comment documenting that this is deliberately avoided), zero
hardcoded credentials/secrets/tokens, and zero unsafe temp-file APIs (`tmpnam` etc.) —
consistent with the argv-only process execution discipline established since Phase 1.

**Third-party licenses**: full inventory in `docs/third-party-licenses.md` — 430 Rust
crates, 179 npm packages, and the 4 vcpkg-managed C++ libraries are all permissively
licensed (MIT/Apache-2.0/BSD/ISC/Zlib/Unlicense, a handful of file-level MPL-2.0 transitive
crates); no GPL/AGPL anywhere in the dependency graph. FFmpeg and yt-dlp are invoked as
separate processes, never linked, with a documented recommendation (an LGPL-only FFmpeg
build) for whoever performs Phase 7's still-outstanding manual binary-sourcing step.

## 8. Known limitations (environment, not defects)

Everything below has been true and documented since Phase 7 and is restated here, not
rediscovered — this phase's job was to confirm nothing regressed, not to pretend a Windows
machine appeared:

- **No actual Windows installer built, installed, or uninstalled.** This environment has no
  Windows machine or NSIS toolchain. Packaging configuration (`tauri.conf.json`) and
  resource-resolution code are complete, reviewed, and unit/E2E-tested on Linux; the
  installer artifact itself is not produced here.
- **FFmpeg and a Python+yt-dlp runtime are not bundled into the installer resources.** The
  mechanism (`scripts/prepare-release-resources.ps1`, `MEDIATOOL_RESOURCE_DIR`) and the
  manual steps are documented; the binaries need a release engineer with a Windows machine.
- **No real YouTube network verification.** Outbound HTTPS to `youtube.com`/`google.com`
  returns a proxy 403 in this sandbox — confirmed directly, not assumed. Every download-path
  test in this project runs against a real subprocess speaking the real protocol
  (`fake_downloader.py`), never against a live network, and that has been the architecture
  since Phase 2 specifically so testing doesn't depend on YouTube being reachable or stable.
- **The orphan-child-process-on-abrupt-parent-kill finding (§4)** needs Windows-specific
  process-lifetime binding to close completely; the graceful paths a real user actually
  triggers (cancel, normal quit) are already correct, and the file-leak half of the same
  scenario is now fixed and tested.
- **Windows-specific rendering** (high-DPI, OS scaling) is unverified — this session's only
  display is a Linux virtual framebuffer, carried forward from Phase 6.

## 9. Version

`1.0.0` across `CMakeLists.txt`, `app/desktop/src-tauri/Cargo.toml` (and its `Cargo.lock`
entry), `app/frontend/package.json`, and `app/desktop/package.json` — confirmed identical by
`scripts/check_versions.py` and confirmed end to end by a clean rebuild's real
`getVersionInfo` response. Kept strictly numeric rather than `1.0.0-rc1`: CMake's own
`project(VERSION ...)` rejects a pre-release suffix outright, and `check_versions.py`'s
cross-check regex is numeric-only by design. The release-candidate designation lives on the
git tag (`v1.0.0-rc1`) and in `docs/release.md` instead — see that document for why this is
a candidate, not a final GA, and exactly what remains before it would become one.

## 10. What Phase 10 did not do

Per its own charter (bug/security/performance fixes and release blockers only, no new major
features): no feature work happened this phase beyond the two fixes in §4, both of which are
real bugs found by real testing, not scope additions.
