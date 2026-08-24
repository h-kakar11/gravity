# Phase 7 — Distribution Engineering

Phase 6 made Gravity look and behave like a finished product. It still only ran from a
development checkout: `mediatool-core`'s default resource paths assumed the process's
current working directory was the repository root, FFmpeg was found only if the developer
happened to have it on `PATH`, the Tauri bundle identifier was still `com.mediatool.desktop`,
and the desktop icon was a flat placeholder square. Phase 7 turns that into something that
could plausibly be installed by a normal user — while being honest, throughout, about which
parts of that claim could actually be verified in this environment.

**This session has no Windows machine.** Gravity targets Windows only (`docs/architecture.md`,
`README.md`). Every code-level fix below was built and tested for real, on Linux, because the
C++ core and the general resource-resolution logic are portable and the project's own dev/CI
flow already runs there (`docs/decisions.md` "Portability fixes"). Producing and testing an
actual `.exe`/NSIS installer was not possible here — see "Known limitations."

## 1. Audit

Read `app/desktop/src-tauri/tauri.conf.json`, `Cargo.toml`, `core_bridge.rs`, `lib.rs`,
`app/core/main.cpp`'s path-resolution helpers, `engines/ffmpeg/FFmpegDiscovery.*`,
`core/settings/*`, `core/queue/QueuePersistence.cpp`, `core/logging/Logger.cpp`, and the
icon set. Traced exactly what happens at each launch mode:

| Launch | Before Phase 7 |
|---|---|
| `npm run tauri dev` | Rust guesses `mediatool-core.exe` at a path relative to `std::env::current_dir()` — correct only when CWD happens to be `app/desktop`, which `docs/development.md`'s documented dev command sets. |
| A packaged install, Start Menu shortcut | The same CWD-relative guess. A shortcut's working directory is whatever Windows/NSIS set it to, not the repo — this would fail outright. Nothing was actually bundled for it to find anyway (`bundle.resources` didn't exist). |
| `mediatool-core.exe` run directly (`--selftest`, tests) | Python and the downloader script defaulted to `python/downloader/.venv/Scripts/python.exe` / `python/downloader/downloader.py`, relative to CWD — works only from the repo root. |
| FFmpeg/ffprobe, any launch | Resolved via an explicit Settings override, or by shelling out to `where` on the user's `PATH` — silently depends on the user having installed FFmpeg themselves. Exactly the pattern spec section 3 warns against. |

## 2. No CWD dependency

`core/filesystem/ExecutablePath.h`/`.cpp` (new) answers "what directory is the currently
running executable actually in" via `GetModuleFileNameW` on Windows and `/proc/self/exe` on
Linux (the platform this repo builds and tests on) — never the process's current working
directory. Covered by `tests/core/ExecutablePathTest.cpp`, which verifies it against the
real, running test binary (there's no meaningful mock for "where am I" — the only honest
test is against reality).

Resource defaults now build from that, not from `std::getcwd()`:

- `ResolvePythonExecutable()`/`ResolveDownloaderScript()` (`app/core/main.cpp`) resolve
  under `<resource dir>/python/` and `<resource dir>/downloader/`.
- FFmpeg/ffprobe discovery (`engines/ffmpeg/FFmpegDiscovery.cpp`) checks
  `<resource dir>/bin/ffmpeg.exe` (and directly beside the executable) before falling back
  to a `PATH` lookup.

**Resource dir**, concretely: the Rust shell is the one piece of the application that
actually knows where Tauri placed `bundle.resources` (`app_handle.path().resource_dir()`),
so it resolves that once at sidecar-spawn time and hands it to `mediatool-core` explicitly
via a `MEDIATOOL_RESOURCE_DIR` environment variable — rather than have the C++ side
re-derive or guess Tauri's packaging convention independently. Absent that variable (the
core binary run directly, e.g. `--selftest`), it falls back to
`<its own executable directory>/resources`. `MEDIATOOL_CORE_PATH`,
`MEDIATOOL_PYTHON_PATH`, and `MEDIATOOL_DOWNLOADER_SCRIPT` remain as explicit development
overrides, unchanged from Phase 1 — `docs/development.md` already documents setting them
by hand for local development rather than relying on either default.

**Verified**: rebuilt the whole C++ suite (352 tests, 0 failures — 6 new), rebuilt the Rust
shell (`cargo build`, clean), and relaunched the real Tauri app under a virtual display
exactly as in Phase 6 with only `MEDIATOOL_CORE_PATH` set (the one override a developer
still needs without a packaged bundle) — it connected to the real core process and rendered
correctly (screenshot taken, matches Phase 6's baseline). What was **not** verified: an
actual Start Menu shortcut, a Desktop shortcut, an install path containing spaces, or a
launch from an arbitrary directory on a real Windows machine — there is no Windows
environment available here. The code no longer has a CWD-relative default in its
production path, which is the actual defect being fixed; exercising every real-world launch
vector needs a Windows box.

## 3. Sidecars — what's bundled, what isn't

| Binary | Status |
|---|---|
| `mediatool-core.exe` | Bundled. `tauri.conf.json`'s `bundle.resources` copies whatever is staged under `app/desktop/src-tauri/resources/` (built by this repo's own CMake, Release config) into the packaged resource directory. |
| The Python downloader script (`downloader.py` + its module) | Bundled the same way — plain text, no reason not to. |
| **FFmpeg / ffprobe** | **Not bundled in this session.** Discovery now checks for a bundled copy first (§2) and documents exactly where a release engineer places one (`app/desktop/src-tauri/resources/README.md`), but this repository does not vendor a Windows FFmpeg build — it's a large (~80MB), independently-versioned, separately-licensed third-party binary (see "License / third-party review" below), and downloading and committing one was out of scope for what this environment could responsibly do unattended. Today's build falls back to a system `PATH` install, same as before Phase 7. |
| **Python + yt-dlp** | **Not bundled.** Same reasoning — an embeddable Python distribution with yt-dlp installed into it is the correct shape for this (`resources/README.md` documents it precisely), but assembling and testing one needs a Windows machine and was not done here. |

This is the single biggest honest gap in this phase: the *mechanism* for shipping FFmpeg
and yt-dlp without asking the user to install them is built and documented, but the actual
binaries were not produced. See "Known limitations."

No development binaries are packaged by accident: `bundle.resources` only ever contains
what `scripts/prepare-release-resources.ps1` explicitly stages from a Release CMake build,
and that directory is gitignored (`app/desktop/src-tauri/resources/*`, keeping only its own
`README.md`) so nothing built locally leaks into a commit.

## 4. Resource discovery strategy (summary)

One rule, stated once, followed everywhere: **an explicit override wins; a bundled copy is
next; a system-wide fallback (PATH, for FFmpeg only) is last resort; nothing defaults to a
path relative to the current working directory.** See §2 for the mechanism and
`core/filesystem/ExecutablePath.h`'s header comment for the canonical statement.

## 5. Windows packaging

`tauri.conf.json`:
- `bundle.targets: ["nsis"]` (unchanged — the project's existing choice).
- `bundle.resources`: `{"resources/**/*": "resources"}`, wiring in the sidecar/FFmpeg/
  Python staging area from §3.
- `bundle.windows.nsis.installMode: "currentUser"` — installs to
  `%LOCALAPPDATA%\Programs`, needing no admin elevation, and matches the app's existing
  choice to keep all mutable state under `%LOCALAPPDATA%` (§6) rather than a
  `Program Files` location a standard user account can't write to.
- `version` is no longer hardcoded here — Tauri inherits it from
  `app/desktop/src-tauri/Cargo.toml`'s `package.version` when the field is omitted (§7),
  confirmed by a clean `cargo build`.

NSIS's own defaults (via `installMode: currentUser`) create a Start Menu shortcut and
register standard Windows "Apps & Features" uninstall metadata (publisher, version,
uninstall command) without any further configuration — this is Tauri/NSIS's documented
stock behavior, not something this phase had to hand-build. **Not verified**: no NSIS
toolchain is available in this Linux environment, so no `.exe` installer was actually
produced or run. See "Known limitations."

## 6. User data vs. application files

Already correct before this phase and unchanged in principle, now consistently named
`Gravity` instead of `MediaTool`:

| What | Where | Survives uninstall? |
|---|---|---|
| `mediatool-core.exe`, bundled FFmpeg/Python, the frontend bundle | The NSIS install directory (`%LOCALAPPDATA%\Programs\Gravity` under `installMode: currentUser`) | No — removed by the uninstaller, as it should be. |
| Settings (`settings.json`) | `%LOCALAPPDATA%\Gravity\settings.json` | Yes — outside the install directory entirely. |
| Queue state (`queue.json`) | `%LOCALAPPDATA%\Gravity\queue.json` | Yes. |
| Logs | `%LOCALAPPDATA%\Gravity\logs\` (rotating, capped at 3×5MB) | Yes. |
| Per-job temp working directories | `%LOCALAPPDATA%\Gravity\temp\job-<id>\` | N/A — cleaned up by the job itself on success or failure; nothing here is meant to persist. |
| **Downloaded/converted media** | Wherever the user pointed `outputDirectory` — never under `%LOCALAPPDATA%` | Always. Not application data at all; the application never writes user media anywhere near its own install or state directories. |

This separation means an uninstall that only removes the install directory (NSIS's
default) cannot touch settings, queue history, logs, or — critically — any media the user
downloaded or produced, which live under an entirely different, user-chosen path. A
reinstall finds its existing `settings.json`/`queue.json` already in place and picks up
where it left off, the same restart-recovery path Phase 5 built and tested (queue
persistence doesn't know or care whether the process restart was a crash or a fresh
install over an old one).

**Renamed** `MediaTool` → `Gravity` in every one of these real path literals
(`JsonFileSettingsStore.cpp`, `QueuePersistence.cpp`, `Logger.cpp`, `TempDirectory.cpp`,
`Settings.cpp`'s default downloads directory) plus their tests, and the `.gitignore` rule
for the local dev-run data directory this creates. This is a real behavior change — a
machine with a pre-Phase-7 dev build's `%LOCALAPPDATA%\MediaTool\` data would not be found
by a Phase-7-or-later build — acceptable because Gravity has not shipped to a real user yet
(see `docs/decisions.md`).

## 7. Versioning

Cargo.toml's `package.version` is the version Tauri and the packaged app actually carry
(tauri.conf.json inherits it by omitting its own `version` field, verified above).
CMakeLists.txt's `project(Gravity VERSION ...)` and `app/frontend/package.json`'s
`"version"` are separate ecosystems' manifests that cannot literally share one file, so
they're kept in agreement by convention and checked mechanically:
`scripts/check_versions.py` reads all three and fails loudly on a mismatch. All three
currently read `0.1.0` (a real version bump to a v1 number is Phase 10's job, not this
one's).

## 8-9. Application identity & icons

- `identifier` changed from `com.mediatool.desktop` to `com.gravity.app` — the Windows
  registry/install-path-facing identifier is not yet "stable" for any real user (Gravity
  has never shipped), so fixing it now, before it locks in, is strictly better than
  carrying the wrong one forward and needing a breaking migration later.
- `productName`/window title were already `"Gravity"` (set in Phase 6).
- Every remaining genuinely user-facing `"MediaTool"` string was found and fixed: the Rust
  panic message on window-build failure, `app/frontend`/`app/desktop`'s `package.json`
  descriptions, `Cargo.toml`'s description, the root `README.md` title, `docs/ipc-contract.md`'s
  title (a live reference doc), and the `--selftest` CLI banner. `mediatool-core`/
  `mediatool_desktop_lib`/`MediaToolException` and similar were deliberately left alone —
  they're internal binary/crate/class names a user never sees, and renaming them now would
  be pure churn across CMake, Cargo, and every doc that names the sidecar binary, for zero
  product-facing benefit (Phase 10 §2's own instruction: "do not blindly replace historical
  references that are technically meaningful").
- The desktop icon (`app/desktop/src-tauri/icons/icon.png` and every generated size/format)
  was a flat, single-color placeholder square — not a real mark. Replaced with an
  on-brand icon (a rounded-square diagonal gradient in the design system's accent colors, a
  simple "gravity well" orbit-and-mass glyph in white) generated at 1024px and run through
  `tauri icon` to produce every required PNG/ICO size. Only the Windows-relevant outputs
  were kept (`32x32.png`, `128x128.png`, `128x128@2x.png`, `icon.ico`, `icon.png`) — the
  tool also generates iOS/Android/Appx assets Gravity doesn't target, which were deleted
  rather than committed as dead weight.

## 10. Release build

**Toolchain**: CMake ≥3.21 + Ninja + MinGW-w64 GCC 13 (C++ core), Rust stable
(`x86_64-pc-windows-gnu`) + the Tauri CLI (shell), Node.js LTS (frontend) — all documented
already in `docs/development.md`, unchanged by this phase.

**Commands, from a clean checkout, on Windows:**
```powershell
# 1. C++ core, Release configuration
cmake --preset windows-mingw-release
cmake --build --preset windows-mingw-release

# 2. Stage this repo's own build outputs for bundling (see §3 for what this does and does not do)
./scripts/prepare-release-resources.ps1

# 3. Frontend + installer
cd app/frontend && npm install && npm run build
cd ../desktop && npm install && npm run build   # `tauri build` -- produces the NSIS installer
```

**Expected output**: `app/desktop/src-tauri/target/release/bundle/nsis/Gravity_<version>_x64-setup.exe`
(Tauri's standard NSIS output location/naming).

Steps 1 and 3's `npm install && npm run build` for the frontend, and the full C++
build/test cycle, were run for real in this session (Linux) and are clean (§ "Regression"
below). Step 2 and the final `tauri build` NSIS packaging step were **not** run for real —
no Windows machine, no NSIS toolchain, no makensis binary available here. The script and
the config were written and reviewed carefully, but "the installer actually gets produced
and actually installs" is unverified. See "Known limitations."

## 11-14. Clean-machine / install-uninstall / upgrade / crash-restart testing

None of these could be performed for real: they all require either a genuine clean Windows
environment or an actual installed build, neither available here. What could be, and was,
verified instead:

- **No accidental dependency on a development environment**: grepped the entire
  production code path (everything reachable from `RunIpcLoop`, not test-only code) for
  anything that assumes a repo-relative path, a dev-only env var without a fallback, or a
  hardcoded absolute path from this session's own filesystem — none found beyond what §2
  already fixed. `docs/development.md`'s dev overrides (`MEDIATOOL_CORE_PATH`,
  `MEDIATOOL_PYTHON_PATH`, etc.) are opt-in, never assumed.
- **Crash/restart recovery** — not re-tested this phase; Phase 5 already built and
  verified this extensively (`docs/phase-5.md`: kill the process mid-download, mid-encode,
  restart, corrupt-file quarantine, interrupted-job recovery) and nothing in Phase 7
  touched `QueuePersistence`'s logic, only its file's directory name (§6). The full C++
  suite, including `QueuePersistenceTest`, still passes.
- **User-data survival across uninstall** — verified by code inspection (§6): nothing in
  the production code path ever writes settings, queue state, logs, or output media inside
  the install directory NSIS would remove.

## 15. Release logging

Unchanged from Phase 2/5's existing design, re-confirmed still correct: rotating file sink
capped at 5MB × 3 files (bounded growth), written to `%LOCALAPPDATA%\Gravity\logs\` (a
normal user always has write access there — never a protected system directory), and
routed to **stderr**, never stdout (`mediatool-core`'s stdout is the NDJSON protocol
channel — a single misdirected log line there was a real Phase 2 bug, documented in
`docs/decisions.md`). No log line dumps a full FFmpeg/yt-dlp command line (spec section 21
— error messages and log text describe outcomes, never raw argv).

## 16. Security (packaging-relevant slice; full adversarial pass is Phase 8)

Reviewed the packaged code path specifically for: development-only flags (`--selftest` is
intentional, documented, human-invoked diagnostics, not a hidden backdoor — it never runs
as part of the normal IPC loop); hardcoded absolute paths from this development machine
(none in production code — the CWD-relative defaults §2 removed were the only ones, and
E2E test scripts intentionally use this session's paths, which is correct, since they're
test code); secrets (none — the application has no accounts, no API keys, no telemetry, by
design, unchanged since Phase 1). A full adversarial security review (IPC fuzzing, shell
injection, path traversal under packaging conditions) is Phase 8's explicit charter, not
repeated here.

## 17. Package size

Not measurable — no installer was actually produced (§10, §11). Documented instead, from
what's known about each component:

| Component | Approximate size | Notes |
|---|---|---|
| `mediatool-core.exe` (Release, MinGW) | A few MB | Statically links reproc++/spdlog/nlohmann_json; not measured on this Linux build (different toolchain), but comparable C++ binaries in this class are typically 2-10MB. |
| Frontend bundle (`app/frontend/dist`) | 17.6 kB CSS + 221 kB JS (66 kB gzipped) | Measured for real (§18) — Phase 6's actual production build. |
| Tauri/WebView2 shell | Small — Tauri on Windows uses the OS's built-in WebView2 runtime rather than bundling a browser engine (unlike Electron), which is the main reason a Tauri app is dramatically smaller than an equivalent Electron one. | |
| **FFmpeg** (if bundled per §3) | ~70-100MB | By far the largest single dependency were it included — a full static FFmpeg build. This is the expected, unavoidable cost of not requiring the user to install FFmpeg themselves. |
| **Python + yt-dlp** (if bundled per §3) | ~15-30MB | An embeddable Python distribution plus yt-dlp and its few dependencies. |

Total for a fully self-contained installer (FFmpeg + Python bundled): realistically in the
100-150MB range, dominated by FFmpeg. This is normal for the category (yt-dlp's own
official Windows builds that bundle ffmpeg are a similar size) and not a reason to cut
functionality — the alternative is requiring the user to install FFmpeg separately, which
is exactly what Phase 7 is trying to stop being the answer.

## 18. Build validation (this session, Linux)

| Check | Result |
|---|---|
| C++ (GoogleTest) | 352 tests (6 new), 337 pass, 15 skipped (Windows-only), 0 fail |
| Python (`unittest`) | 24 pass, unchanged |
| Frontend (`vitest`) | 75 pass, unchanged from Phase 6 |
| `tsc --noEmit` | clean |
| `vite build` | clean |
| `cargo build` (Tauri shell) | clean |
| `scripts/check_versions.py` | all three manifests agree (`0.1.0`) |
| Real Tauri app launch (Xvfb, real core process) | Home screen rendered and connected, matching Phase 6's baseline, after the resource-resolution refactor |
| **`tauri build` (NSIS installer)** | **Not run** — no NSIS toolchain / Windows environment in this session |

## Known limitations

1. **No installer was actually produced.** The configuration, the resource-bundling
   mechanism, and the staging script are all in place and reviewed, but `tauri build`'s
   NSIS packaging step needs a Windows machine (or a properly configured cross-compilation
   toolchain this environment does not have) and was never run. This is the single largest
   gap in the phase's own definition of done ("a user can receive the packaged application
   and use it").
2. **FFmpeg and a Python+yt-dlp runtime are not actually bundled.** The discovery code
   prefers a bundled copy and falls back to the system `PATH` exactly as before Phase 7;
   the mechanism and the exact manual steps to populate it are documented
   (`resources/README.md`), but the binaries themselves were not sourced and placed —
   doing so responsibly needs a real release engineer with a Windows machine to download,
   verify, and pin specific FFmpeg/yt-dlp versions, and this session had no way to do that
   safely or verify the result.
3. **No install/uninstall/upgrade/clean-machine testing happened for real** — everything
   in §11-14 beyond code inspection is unverified. A genuine Windows test pass is a
   prerequisite before this could be called release-ready (Phase 10 will need to either
   perform this or explicitly carry it forward as a release blocker).
4. **Package size is estimated, not measured**, for the same reason.
5. **A pre-Phase-7 dev build's local `%LOCALAPPDATA%\MediaTool\` data will not be picked
   up** by this or a later build (§6) — acceptable pre-v1, called out explicitly so it
   isn't mistaken for a bug later.

Phase 8 was not started.
