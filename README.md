# MediaTool

A local-first media downloader, converter, and compressor for Windows. No accounts, no
telemetry, no cloud processing — everything runs on your machine (spec sections 24, 34).

**This repository is Phase 1: the foundation only.** It proves the architecture works
end-to-end; it is not a feature-complete or visually finished application yet. See
"Phase 1 status" below for exactly what's real vs. scaffolded, and `docs/roadmap.md` for
what's planned.

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

## Phase 1 status

Per spec section 43: don't claim something works just because its interface exists. This
table is the authoritative "is it real" answer — everything in it was verified by actually
running the code, not just reading it.

| Component | Status |
|---|---|
| Job abstraction, `JobStateMachine`, `JobManager` | **Working.** Configurable concurrency (not hardcoded to 1), pause/resume/cancel/retry all implemented and tested. |
| `TestJob` | **Working.** The Phase 1 proof job — verified running to completion through `JobManager` and via `--selftest`. |
| `DownloadJob` / `ConversionJob` / `CompressionJob` / `BatchJob` / `WorkflowJob` | **Scaffolded only.** Named in the type system (`JobType`); `createJob` returns an honest "not implemented in Phase 1" error for anything but `TEST`. |
| Event system (`EventBus`, `Event`) | **Working.** Synchronous pub/sub, wired to the IPC layer and the logger. |
| Universal `Progress` model | **Working.** |
| Structured `ErrorInfo` | **Working.** |
| Filesystem abstraction (`LocalFileSystem`, `FilenameSanitizer`, `TempDirectory`, `AtomicWriter`) | **Working.** Verified against real files/directories in tests. |
| Hardware detection (`WindowsHardwareDetector`) | **Working.** Verified against real hardware via `--selftest` (correctly detected the dev machine's actual CPU and GPU without any hardcoding). NVENC/QSV/AMF encoder capability listing is a documented Phase 2 TODO — `availableEncoders` is currently always empty. |
| Settings (`JsonFileSettingsStore`) | **Working.** File-backed, atomic writes, defaults-on-missing-file. |
| Logging | **Working.** Rotating file + console sinks via spdlog, routed through one facade. |
| FFmpeg engine — discovery, version, `Probe()` | **Working.** Verified with a real ffmpeg/ffprobe install via `--selftest` (generates a test clip and probes it). |
| FFmpeg engine — `Convert`/`Compress`/`ExtractAudio`/`ExtractFrames` | **Scaffolded only.** Declared on `IMediaEngine`; each throws an honest "not implemented in Phase 1" error. |
| Image engine (`IImageEngine`) | **Interface only**, no implementation (deliberately, per spec section 2). |
| Document engine (`IDocumentConverter`) | **Interface only**, no implementation (deliberately, per spec section 2). |
| Python downloader (`downloader.py`) | **Working.** `--selftest` mode verified (no network); real yt-dlp download path is implemented but only manually smoke-testable (needs network + a real URL, not exercised by automated tests). |
| `YtDlpProvider` (C++ side) | **Working.** Verified end-to-end via `--selftest`, which launches the real Python process and parses its real NDJSON output. |
| Process abstraction (`IProcessRunner`, `RealProcessRunner`, `MockProcessRunner`) | **Working.** `RealProcessRunner` backed by reproc++; a genuine race condition between polling and wait/kill (found and fixed during integration — see `docs/development.md`) made cancellation take ~30s instead of instant. Now verified to cancel in well under a second. |
| IPC boundary (`mediatool-core` NDJSON loop, Tauri Rust bridge, `coreClient.ts`) | **Working.** All 12 commands from `docs/ipc-contract.md` implemented and reachable; verified frontend build + C++ side independently, full three-process round trip not yet manually clicked through in a running window (see Known issues). |
| Frontend developer console | **Working** as a build (`tsc --noEmit && vite build` passes). Deliberately minimal — proves IPC, is not the final UI (spec section 34). |
| Final polished UI (dark mode, two-card home screen) | **Not started**, by design. Requirements recorded in `docs/roadmap.md` for when that phase begins. |

## Tests

123 GoogleTest cases (C++) + a Python `unittest` suite, all passing. See
`docs/development.md` for exact commands.

## Documentation

- `docs/architecture.md` — how the pieces fit together and why
- `docs/ipc-contract.md` — the wire protocol (read this before touching any cross-process code)
- `docs/development.md` — setup, build, test, and run instructions
- `docs/roadmap.md` — what's planned for later phases
