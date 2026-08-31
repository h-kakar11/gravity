# Gravity

A local-first media downloader, converter, and compressor for Windows. No accounts, no
telemetry, no cloud processing — everything runs on your machine (spec sections 24, 34).

Repository: [github.com/h-kakar11/gravity](https://github.com/h-kakar11/gravity) (private).

**Phase 1** built the foundation (Tauri/React shell, C++ core, job system, IPC, FFmpeg
discovery, Python downloader scaffold). **Phase 2** turned the download side into a real,
working vertical slice: URL → metadata inspection → quality selection → real download →
live progress → cancellation → verified output → completed job, end-to-end through the
real architecture (no simulated progress, no bypassed layers). Phases 3 through 5.4 (see
git history) built out the rest of the product on top of that foundation: a real
Convert/Compress engine, a dark-themed multi-screen UI (Home, Download, Convert, Queue,
Settings, Scheduled Tasks), system tray, global hotkeys, multi-profile presets, watch
folders, scheduled tasks, hardware-accelerated encoding, Windows Explorer context-menu
integration, CI, and installer packaging. It is still not feature-complete against the
full product vision — see "Status" below for exactly what's real vs. scaffolded,
`docs/phase-2.md` for the Phase 2 report, and `docs/roadmap.md` for what's still planned
(note: `docs/roadmap.md` itself predates Phases 3-5 and is stale in the same way this file
was until this pass — treat "planned" items there as needing a fresh look, not gospel).

## Architecture at a glance

```
React / TypeScript  <-- Tauri IPC -->  Rust (Tauri shell)  <-- stdio NDJSON -->  C++ core (mediatool-core.exe)
                                                                                        |
                                                                          stdio NDJSON  |
                                                                                        v
                                                                        Python downloader (yt-dlp)
```

See `docs/architecture.md` for the full design and `docs/ipc-contract.md` for the exact
wire protocol every hop above uses.

## Quick start

Prerequisites and full instructions: `docs/development.md`. To validate a change, run
`.\scripts\ci-local.ps1` (see `docs/local-ci.md`) — it mirrors CI's checks locally, no
GitHub Actions runner needed. Short version of building/running by hand:

```bash
# C++ core
cmake --preset windows-mingw-debug
cmake --build --preset windows-mingw-debug
ctest --preset windows-mingw-debug

# Prove the whole backend works end-to-end, no Tauri/React needed:
./build/windows-mingw-debug/app/core/mediatool-core.exe --selftest

# Desktop app (needs the C++ core built above)
cd app/frontend && npm install && cd ../desktop
npm install && npm run tauri dev
```

If `npm run tauri dev` panics with "failed to spawn mediatool-core... The system cannot
find the path specified", the dev-mode relative-path guess in `core_bridge.rs` guessed
wrong for how your Tauri CLI version invokes `cargo run` (see `docs/development.md`) — set
`MEDIATOOL_CORE_PATH` (and, if it still can't find Python, `MEDIATOOL_PYTHON_PATH`/
`MEDIATOOL_DOWNLOADER_SCRIPT`) to absolute paths before running the command.

## Repository structure

```
app/
  frontend/     React + TypeScript dark-themed multi-screen UI (Home, Download, Convert,
                Queue, Settings, Scheduled Tasks) -- see "Status" below
  desktop/      Tauri (Rust) shell -- bridges frontend <-> mediatool-core
  core/         The mediatool-core sidecar executable's entry point (main.cpp)
core/           C++ abstractions: jobs, events, filesystem, process, hardware, settings, logging, errors
engines/        FFmpeg, image, document, and downloader engine implementations
python/         The yt-dlp-based downloader subsystem
tests/          GoogleTest (C++) and unittest (Python) suites
docs/           architecture.md, development.md, roadmap.md, ipc-contract.md
third_party/    vcpkg (gitignored -- clone it yourself, see docs/development.md)
```

## Status

Per spec section 43: don't claim something works just because its interface exists. This
table is the authoritative "is it real" answer — everything in it was verified by actually
running the code, not just reading it (Phase 2's `DownloadJob` row was verified against
real YouTube videos with real network access — see `docs/phase-2.md` for the exact runs).

| Component | Status |
|---|---|
| Job abstraction, `JobStateMachine`, `JobManager`, `SchedulerCore` | **Working.** Configurable concurrency (not hardcoded to 1), pause/resume/cancel/retry all implemented and tested. State transitions report their outcome instead of throwing, so a cancellation racing a start is a value the worker inspects rather than an exception that kills it. Scheduling (priority ordering, job dependencies with failure propagation) lives in a threadless `SchedulerCore` and is tested without threads. See `docs/concurrency-model.md`. |
| `TestJob` | **Working.** The Phase 1 proof job — verified running to completion through `JobManager` and via `--selftest`. |
| `DownloadJob` | **Working.** Real end-to-end downloads verified against live YouTube videos (audio-only and a 480p video+audio merge), including cancellation mid-download and output verification via `ffprobe`. Pause is not supported (spec doesn't require it for downloads). Playlist URLs are deliberately rejected, not partially supported. |
| `MediaProcessingJob` (Convert and Compress) | **Working**, added in Phase 2.6. One job class/code path for both — Compress is Convert with different default option values (`docs/decisions.md`), not a separate implementation. Backed by `FFmpegEngine::Convert`/`Compress`. |
| `BatchJob` / `WorkflowJob` | **Not implemented.** `JobType::Batch`/`Workflow` exist as enum values (`core/jobs/JobTypes.h`) but there's no corresponding `Job` subclass — `createJob` rejects both at creation. A bigger architectural item (audit issue #17). |
| Event system (`EventBus`, `Event`) | **Working.** Synchronous pub/sub, wired to the IPC layer and the logger. |
| Universal `Progress` model | **Working.** |
| Structured `ErrorInfo` | **Working.** |
| Filesystem abstraction (`LocalFileSystem`, `FilenameSanitizer`) | **Working.** Verified against real files/directories in tests. |
| `TempDirectory`, `AtomicWriter` | **Implemented and unit-tested, but not wired into any job's production path** (`DownloadJob`, `MediaProcessingJob`) — see `docs/architecture.md`'s "Filesystem, temp files, and atomic output" section for what that means for crash safety today. Tracked under issue #10. |
| Hardware detection (`WindowsHardwareDetector`) | **Working.** Verified against real hardware via `--selftest` (correctly detected the dev machine's actual CPU and GPU without any hardcoding). NVENC/QSV/AMF encoder capability listing is a documented Phase 2 TODO — `availableEncoders` is currently always empty. |
| Settings (`JsonFileSettingsStore`) | **Working.** File-backed, atomic writes, defaults-on-missing-file. |
| Logging | **Working.** Rotating file sink + a **stderr** color sink via spdlog (fixed in Phase 2 — it originally wrote to stdout, which is reserved for the NDJSON protocol; see `docs/decisions.md`/`docs/phase-2.md` known issues), routed through one facade. |
| FFmpeg engine — discovery, version, `Probe()` | **Working.** Verified with a real ffmpeg/ffprobe install via `--selftest` (generates a test clip and probes it). |
| FFmpeg engine — `Convert`/`Compress` | **Working**, added in Phase 2.6. Backs `MediaProcessingJob`; `getCapabilities` reflects real availability. |
| FFmpeg engine — `ExtractAudio`/`ExtractFrames` | **Scaffolded only.** Still declared on `IMediaEngine`, still throws `E_NOT_IMPLEMENTED`. |
| Image engine (`IImageEngine`) | **Interface only**, no implementation (deliberately, per spec section 2). |
| Document engine (`IDocumentConverter`) | **Interface only**, no implementation (deliberately, per spec section 2). |
| Python downloader (`downloader.py`) | **Working.** Both `inspect` and `download` commands verified against real, live YouTube videos with real network access — rich metadata with full format lists, real progress, real yt-dlp+ffmpeg merge, real error classification. `--selftest` mode still covers the no-network case for automated tests. |
| `YtDlpProvider` (C++ side, incl. `Inspect()`, `YtDlpFormatSelector`) | **Working.** Verified via `--selftest` (Phase 1 path) and via a direct real IPC session driving `inspectDownloadUrl`/`createJob{DOWNLOAD}` against live YouTube URLs (Phase 2). |
| `QualityPreset` / format selection abstraction | **Working.** `BEST`/`2160P`/`1440P`/`1080P`/`720P`/`480P`/`AUDIO_ONLY`; translated to a concrete yt-dlp selector only inside `engines/downloader/YtDlpFormatSelector`. |
| Filename collision handling (`DeduplicateBaseName`) | **Working.** Verified both in unit tests and for real — a second real download of the same video during manual testing correctly produced "... (1).webm" instead of overwriting. |
| Output verification (`ffprobe` round-trip) | **Working.** A completed `DownloadJob` is only marked `COMPLETED` after `FileInfo`/`ffprobe` confirms a non-empty, valid media file — verified with real downloaded files. |
| Process abstraction (`IProcessRunner`, `RealProcessRunner`, `MockProcessRunner`) | **Working.** `RealProcessRunner` backed by reproc++; a genuine race condition between polling and wait/kill (found and fixed during Phase 1 integration — see `docs/development.md`) made cancellation take ~30s instead of instant. Now verified to cancel in well under a second, including a real in-flight download cancellation in Phase 2. |
| Filesystem abstraction, incl. `ListDirectory` and `MockFileSystem` | **Working.** `MockFileSystem` added in Phase 2 to round out spec section 39's three named mocks (process runner, filesystem, downloader provider). |
| `MockDownloadProvider` | **Working.** Added in Phase 2; used by `DownloadJobTest` so no test needs real yt-dlp or network access. |
| IPC boundary (`mediatool-core` NDJSON loop, Tauri Rust bridge, `coreClient.ts`) | **Working.** 13 commands from `docs/ipc-contract.md` (12 from Phase 1 + `inspectDownloadUrl`) implemented and reachable. Verified two ways: a real Tauri window + sidecar launch (both processes running simultaneously), and a direct NDJSON session driving real downloads through the same `mediatool-core.exe` binary the Tauri shell spawns. |
| Frontend: dark-themed multi-screen UI | **Working**, built across Phases 2.2-5.4. Home (idealist.md's two-card layout), Download, Convert & Compress, unified Queue (active + history), Settings, and Scheduled Tasks, behind a shared `AppShell` nav shell — not the Phase 1/2 two-tab dev console this row used to describe. `DownloaderPage.tsx` specifically retains some Phase-2-era inline light styling rather than the shared dark theme (tracked, not yet ported). |
| Frontend: Dev console (`DevConsole.tsx`) | **Working**, but no longer a product surface -- a dev-only diagnostic reachable via `?devConsole=1` in `npm run dev`, kept for proving raw IPC without the real UI in the way. |

## Tests

146 GoogleTest cases (C++) + a Python `unittest` suite (24 cases across two files), all
passing. See `docs/development.md` for exact commands, and `docs/protocols/downloader.md`
for the manual (non-automated) real-network integration test procedure.

## Documentation

- `docs/architecture.md` — how the pieces fit together and why
- `docs/ipc-contract.md` — the wire protocol (read this before touching any cross-process code)
- `docs/protocols/downloader.md` — the Python downloader sub-protocol in full detail
- `docs/development.md` — setup, build, test, and run instructions
- `docs/local-ci.md` — running the full validation suite locally, no GitHub Actions needed
- `docs/decisions.md` — architectural decisions and why they were made
- `docs/roadmap.md` — what's planned for later phases
- `docs/phase-2.md` — the Phase 2 engineering report
