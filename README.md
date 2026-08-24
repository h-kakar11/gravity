# MediaTool ("gravity")

A local-first media downloader, converter, and compressor for Windows. No accounts, no
telemetry, no cloud processing — everything runs on your machine (spec sections 24, 34).

Repository: [github.com/h-kakar11/gravity](https://github.com/h-kakar11/gravity) (private).

**Phase 1** built the foundation (Tauri/React shell, C++ core, job system, IPC, FFmpeg
discovery, Python downloader scaffold). **Phase 2** turned the download side into a real,
working vertical slice: URL → metadata inspection → quality selection → real download →
live progress → cancellation → verified output → completed job, end-to-end through the
real architecture (no simulated progress, no bypassed layers). It is still not the final,
polished application — see "Status" below for exactly what's real vs. scaffolded,
`docs/phase-2.md` for the full Phase 2 report, and `docs/roadmap.md` for what's planned.

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

Prerequisites and full instructions: `docs/development.md`. Short version:

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
  frontend/     React + TypeScript developer console (not the final UI yet)
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
running the code, not just reading it. Phase 2's `DownloadJob` row was verified against real
YouTube videos with real network access (`docs/phase-2.md`); Phase 5's rows were verified by
driving the real `mediatool-core` binary with real FFmpeg (`docs/phase-5.md`, and the
verification matrix at the end of it).

| Component | Status |
|---|---|
| Job abstraction, `JobStateMachine`, `JobManager` | **Working.** Configurable concurrency (not hardcoded to 1), pause/resume/cancel/retry all implemented and tested. |
| `TestJob` | **Working.** The Phase 1 proof job — verified running to completion through `JobManager` and via `--selftest`. |
| `DownloadJob` | **Working.** Real end-to-end downloads verified against live YouTube videos (audio-only and a 480p video+audio merge), including cancellation mid-download and output verification via `ffprobe`. Pause is not supported (spec doesn't require it for downloads). Playlist URLs are deliberately rejected, not partially supported. |
| `ConversionJob` / `CompressionJob` | **Working.** Real FFmpeg conversion (10 target formats) and quality-preset compression with optional downscaling, verified end-to-end against real media — including that a compression asked for 120px output actually produced 120px. Output is written atomically and probed back before being committed. |
| `BatchJob` / `WorkflowJob` | **Scaffolded only.** Named in the type system (`JobType`); `createJob` returns an honest "not implemented yet" error for these. |
| Unified job queue (`SchedulerCore`, queue side of `JobManager`) | **Working.** One queue across all job types: configurable runtime concurrency, priorities, reordering, dependencies, bounded retries with classification and backoff, queue pause/resume, bounded history. Verified end-to-end, including that the concurrency limit caps real `ffmpeg` child processes and not merely job objects. |
| Queue persistence & restart recovery (`QueuePersistence`) | **Working.** Versioned JSON written atomically; verified by killing the core process and restarting it. A corrupt state file is quarantined and the app starts anyway — verified with a deliberately truncated file. |
| Dependency pipelines (`download -> convert -> compress`) | **Working.** A stage names the job it reads from and the backend resolves the real path once that job has run — verified against a download whose filename nothing could have predicted. A failed dependency marks its dependents `SKIPPED`, not `FAILED`. |
| Output name reservation (`OutputNameRegistry`) | **Working.** Stops concurrent jobs picking the same filename. Added after reproducing real data loss: three same-title downloads all reporting success with one file on disk. |
| Event system (`EventBus`, `Event`) | **Working.** Synchronous pub/sub, wired to the IPC layer and the logger. |
| Universal `Progress` model | **Working.** |
| Structured `ErrorInfo` | **Working.** |
| Filesystem abstraction (`LocalFileSystem`, `FilenameSanitizer`, `TempDirectory`, `AtomicWriter`) | **Working.** Verified against real files/directories in tests. |
| Hardware detection (`WindowsHardwareDetector`) | **Working.** Verified against real hardware via `--selftest` (correctly detected the dev machine's actual CPU and GPU without any hardcoding). NVENC/QSV/AMF encoder capability listing is a documented Phase 2 TODO — `availableEncoders` is currently always empty. |
| Settings (`JsonFileSettingsStore`) | **Working.** File-backed, atomic writes, defaults-on-missing-file. |
| Logging | **Working.** Rotating file sink + a **stderr** color sink via spdlog (fixed in Phase 2 — it originally wrote to stdout, which is reserved for the NDJSON protocol; see `docs/decisions.md`/`docs/phase-2.md` known issues), routed through one facade. |
| FFmpeg engine — discovery, version, `Probe()` | **Working.** Verified with a real ffmpeg/ffprobe install via `--selftest` (generates a test clip and probes it). |
| FFmpeg engine — `Convert`/`Compress` | **Working.** Real encodes with `-progress` parsing, bounded terminate-then-kill cancellation, a stall watchdog, and atomic committed output. `FFmpegArgumentBuilder` builds every `argv` and is unit-tested without launching anything. |
| FFmpeg engine — `ExtractAudio`/`ExtractFrames` | **Scaffolded only.** Declared on `IMediaEngine`; each throws an honest "not implemented" error. |
| Image engine (`IImageEngine`) | **Interface only**, no implementation (deliberately, per spec section 2). |
| Document engine (`IDocumentConverter`) | **Interface only**, no implementation (deliberately, per spec section 2). |
| Python downloader (`downloader.py`) | **Working.** Both `inspect` and `download` commands verified against real, live YouTube videos with real network access — rich metadata with full format lists, real progress, real yt-dlp+ffmpeg merge, real error classification. `--selftest` mode still covers the no-network case for automated tests. |
| `YtDlpProvider` (C++ side, incl. `Inspect()`, `YtDlpFormatSelector`) | **Working.** Verified via `--selftest` (Phase 1 path) and via a direct real IPC session driving `inspectDownloadUrl`/`createJob{DOWNLOAD}` against live YouTube URLs (Phase 2). |
| `QualityPreset` / format selection abstraction | **Working.** `BEST`/`2160P`/`1440P`/`1080P`/`720P`/`480P`/`AUDIO_ONLY`; translated to a concrete yt-dlp selector only inside `engines/downloader/YtDlpFormatSelector`. |
| Filename collision handling (`DeduplicateBaseName`, `OutputNameRegistry`) | **Working.** Verified both in unit tests and for real — a second real download of the same video correctly produced "... (1).webm" instead of overwriting, and three *concurrent* same-title downloads now produce three distinct files (they did not before Phase 5 — see `docs/phase-5.md`). |
| Output verification (`ffprobe` round-trip) | **Working.** A completed `DownloadJob` is only marked `COMPLETED` after `FileInfo`/`ffprobe` confirms a non-empty, valid media file — verified with real downloaded files. |
| Process abstraction (`IProcessRunner`, `RealProcessRunner`, `MockProcessRunner`) | **Working.** `RealProcessRunner` backed by reproc++; a genuine race condition between polling and wait/kill (found and fixed during Phase 1 integration — see `docs/development.md`) made cancellation take ~30s instead of instant. Now verified to cancel in well under a second, including a real in-flight download cancellation in Phase 2. |
| Filesystem abstraction, incl. `ListDirectory` and `MockFileSystem` | **Working.** `MockFileSystem` added in Phase 2 to round out spec section 39's three named mocks (process runner, filesystem, downloader provider). |
| `MockDownloadProvider` | **Working.** Added in Phase 2; used by `DownloadJobTest` so no test needs real yt-dlp or network access. |
| IPC boundary (`mediatool-core` NDJSON loop, Tauri Rust bridge, `coreClient.ts`) | **Working.** 23 commands from `docs/ipc-contract.md` implemented and reachable. Verified two ways: a real Tauri window + sidecar launch, and direct NDJSON sessions driving real downloads, encodes and every queue command through the same binary the Tauri shell spawns. All input is validated at the boundary — 15 malformed-input rejections are asserted end-to-end. |
| Frontend: Download page (`DownloaderPage.tsx`) | **Working** as a build (`tsc --noEmit && vite build` pass) and wired to real backend events end-to-end — no simulated progress. The live click-through in a running Tauri window wasn't captured by this session's screen-automation tooling (an environment/tooling limitation, not a code issue — see `docs/phase-2.md` known issues); the same backend calls it makes were separately verified for real via a direct NDJSON session. |
| Frontend: Queue page + `queueReducer` | **Working.** Filters, sorting, per-job and queue-level controls, a job detail panel, and statistics, over a store that reconciles a snapshot with incremental events using both a channel sequence number and a per-job revision. 57 `vitest` tests cover the reducer and the display/control-availability helpers. |
| Frontend: Convert & Compress page | **Working** as a build, wired to the real backend. Builds the two-stage pipeline by declaring the link, not by polling. |
| Frontend: Dev console | **Working** as a build. Deliberately minimal — proves IPC, is not the final UI (spec section 34). |
| Final polished UI (dark mode, two-card home screen) | **Not started**, by design. Requirements recorded in `docs/roadmap.md` for when that phase begins. |

## Tests

| Suite | Count | Status |
|---|---|---|
| C++ (GoogleTest) | 345 | 330 pass, 15 skipped (Windows-only assertions, on a Linux run), 0 fail |
| Python (`unittest`) | 24 | pass |
| Frontend (`vitest`) | 57 | pass |
| End-to-end, real binary + real FFmpeg | 109 checks | pass |

See `docs/development.md` for exact commands, `tests/e2e/README.md` for what the end-to-end
suites do and deliberately do not cover, and `docs/protocols/downloader.md` for the manual
real-network integration test.

## Documentation

- `docs/architecture.md` — how the pieces fit together and why
- `docs/ipc-contract.md` — the wire protocol (read this before touching any cross-process code)
- `docs/protocols/downloader.md` — the Python downloader sub-protocol in full detail
- `docs/development.md` — setup, build, test, and run instructions
- `docs/decisions.md` — architectural decisions and why they were made
- `docs/roadmap.md` — what's planned for later phases
- `docs/phase-2.md` — the Phase 2 engineering report
- `docs/phase-5.md` — the Phase 5 engineering report (the queue: architecture, retry policy,
  recovery, the bugs found along the way, and the verification matrix)
- `tests/e2e/README.md` — the end-to-end queue harnesses
