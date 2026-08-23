# Architecture

## Overview

```
React / TypeScript  <-- Tauri IPC -->  Rust (Tauri shell)  <-- stdio NDJSON -->  C++ core (mediatool-core.exe)
                                                                                        |
                                                                          stdio NDJSON  |
                                                                                        v
                                                                        Python downloader (yt-dlp)
```

Three processes, two languages beyond TypeScript, one wire protocol. Every hop uses the
same framing rules — see `docs/ipc-contract.md`, which is the authoritative naming/schema
source and should be read before touching any of the code below.

## Why a sidecar process instead of Rust↔C++ FFI

The obvious alternative is compiling the C++ core as a static/shared library and linking
it into the Rust binary via `cxx` or `bindgen`. Phase 1 uses a standalone `mediatool-core`
executable that the Tauri shell spawns and talks to over stdio instead, because:

- It matches the process-abstraction philosophy already required for FFmpeg and the
  Python downloader (spec section 2: "prefer invoking the executable through a
  controlled process abstraction rather than tightly coupling").
- It sidesteps Rust/C++ ABI and cross-compiler-toolchain concerns entirely (the C++ core
  is built with MinGW-w64 GCC; Rust uses its own toolchain) — no name-mangling or calling
  convention negotiation needed.
- The C++ core stays independently runnable, debuggable, and testable without Tauri or
  Rust in the loop at all — `mediatool-core.exe --selftest` proves the whole backend
  works before a single line of Rust or React runs.

The cost is one process-spawn and a JSON-parsing hop per command, which is irrelevant for
an application whose operations (downloads, encodes) are measured in seconds to minutes.

## Frontend/backend separation

The frontend **never** executes FFmpeg or yt-dlp, and contains **no** media-processing
logic (spec section 2). It only knows how to call `sendCommand(...)` and subscribe to
`core-event` (see `app/frontend/src/services/coreClient.ts` and
`app/frontend/src/types/ipc.ts`). Every type the frontend uses to describe a job, an
error, a file, or a hardware profile is a hand-mirrored copy of the corresponding C++
header — see `docs/ipc-contract.md`'s "Shared types" section for the field-by-field
mapping and the rule for keeping both sides in sync.

## The job system

Everything the application does is eventually a `Job` (spec section 4):

```
Job (core/jobs/Job.h)
 +-- DownloadJob        (Phase 2+)
 +-- ConversionJob      (Phase 2+)
 +-- CompressionJob     (Phase 2+)
 +-- BatchJob           (Phase 2+)
 +-- WorkflowJob        (Phase 2+)
 \-- TestJob             Phase 1 — synthetic, proves the pipeline end-to-end
```

`JobStateMachine` (`core/jobs/JobStateMachine.h`) is the only thing allowed to decide
whether a state transition is valid — see `docs/ipc-contract.md` for the transition
table. `JobManager` (`core/jobs/JobManager.h`) owns job lifecycle (create, queue, start,
track, pause/resume where supported, cancel, retry, remove) against a configurable
`maxConcurrentJobs`, deliberately not hardcoded to 1 even though Phase 1 runs with a
concurrency of 1 — batch processing and concurrent downloads in later phases raise this
without any structural change (spec section 6).

## The event system

`EventBus` (`core/events/EventBus.h`) is a synchronous pub/sub bus. `JobManager` and the
logger publish structured `Event`s (`core/events/Event.h`); the `mediatool-core`
executable's IPC loop subscribes and turns them into NDJSON lines on stdout. The frontend
never parses human-readable log text to infer state (spec section 8) — every state change
and progress update is a typed event with a fixed JSON shape.

## The universal progress model

`Progress` (`core/jobs/Progress.h`) is one struct used for every kind of operation —
byte-oriented downloads, frame/time-oriented encodes, and count-oriented batch jobs — by
making every field except `statusMessage` optional. See spec section 7 and
`docs/ipc-contract.md`.

## FFmpeg integration

`engines/ffmpeg/FFmpegEngine` implements `core/media/IMediaEngine.h` and is the *only*
thing in the codebase allowed to invoke `ffmpeg`/`ffprobe`, always through
`core/process/IProcessRunner.h` with a structured argv vector — never a shell string
(spec section 16). `FFmpegDiscovery` finds the binaries (or reports their absence without
throwing); `FFmpegProgressParser` turns ffmpeg's `-progress pipe:1` output into a
`Progress` and is unit-tested with canned text, with no real ffmpeg process required
(spec section 36). Actual `Convert`/`Compress`/`ExtractAudio`/`ExtractFrames` are declared
on the interface but intentionally unimplemented in Phase 1 — see "What's scaffolded vs.
working" below.

## Python downloader

`python/downloader/downloader.py` is the only thing in the codebase that imports
`yt_dlp`. `engines/downloader/YtDlpProvider` implements `core/downloads/IDownloadProvider.h`
by launching it through an `IProcessRunner` and translating its NDJSON events
(`docs/ipc-contract.md`) into the same `Progress`/callback shapes every other engine uses
— the rest of the codebase never needs to know yt-dlp exists (spec section 19; a future
`VimeoProvider` or similar plugs into the same interface).

## Filesystem, temp files, and atomic output

`core/filesystem/LocalFileSystem` implements `IFileSystem` on top of `std::filesystem`
(never manual path string concatenation, spec section 11). `TempDirectory` gives each job
an isolated working directory under `%LOCALAPPDATA%\MediaTool\temp\job-<id>\`; `AtomicOutput`
implements the "write to a `.processing` file, rename over the real path only on success"
pattern (spec section 13) so a crash mid-operation never destroys or corrupts a user's
file. `FilenameSanitizer` is the one place Windows-illegal-filename handling lives — the
UI never sanitizes filenames itself (spec section 21).

## Hardware detection

`core/hardware/WindowsHardwareDetector` reports CPU core count/name and enumerates GPU
adapters via DXGI, classified by PCI vendor ID. It never assumes NVIDIA is present (spec
section 22) and never throws — an empty GPU list or empty encoder list is a valid result
on any machine.

## Testing strategy

Every cross-process or cross-hardware dependency sits behind one of five interfaces (spec
section 37): `IProcessRunner`, `IMediaEngine`, `IDownloadProvider`, `IFileSystem`,
`IClock`. Production code depends on the interface; tests depend on a `Mock*`
implementation. No unit test needs a real ffmpeg, a real network connection, or the real
`%LOCALAPPDATA%` to pass — see `docs/development.md`'s testing section for how to run the
suites.

## What's scaffolded vs. working

See the root `README.md`'s "Phase 1 status" table for the authoritative, per-component
breakdown — do not assume a class exists just because its header does; several interfaces
in this document (`IImageEngine`, `IDocumentConverter`, `Convert`/`Compress` on
`IMediaEngine`) are declared but intentionally not implemented yet.
