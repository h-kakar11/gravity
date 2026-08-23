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
 +-- DownloadJob        Phase 2 — real, see "The download pipeline" below
 +-- ConversionJob      (Phase 3+)
 +-- CompressionJob     (Phase 3+)
 +-- BatchJob           (Phase 3+)
 +-- WorkflowJob        (Phase 3+)
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

## The download pipeline (Phase 2)

```
React (DownloaderPage) -> Tauri -> C++ core -> DownloadJob -> IDownloadProvider (YtDlpProvider)
                                                                       |
                                                              stdio NDJSON
                                                                       v
                                                        python/downloader/downloader.py -> yt-dlp -> ffmpeg
```

Two IPC commands drive it (`docs/ipc-contract.md`):

- `inspectDownloadUrl(url)` — a direct, synchronous request/response call (not a Job) that
  fetches title/uploader/duration/thumbnail/available formats without downloading
  anything. It blocks the IPC loop for the duration of the network probe (typically 1-3s)
  — see "Known limitations" below.
- `createJob({type: "DOWNLOAD", params: {url, outputDirectory, quality}})` — creates a real
  `DownloadJob` (`core/jobs/DownloadJob.h`) that runs on the existing `JobManager` worker
  pool, same as `TestJob`. No separate downloader-specific queue exists (spec section 38).

`DownloadJob::Execute()` does, in order: (1) call `IDownloadProvider::Inspect()` again
(deliberately re-fetching, not trusting whatever the frontend's earlier `inspectDownloadUrl`
call saw — keeps the job self-contained, see `docs/decisions.md`) to get the current
title; (2) sanitize it (`FilenameSanitizer::SanitizeWindowsFilename`) and pick a
collision-free base filename via `FilenameSanitizer::DeduplicateBaseName` (checked against
*any* extension, since the final container isn't known until after the download — spec
section 29); (3) call `IDownloadProvider::Download()`, forwarding `Progress` updates
straight through to `ReportProgress()`; (4) verify the result — file exists, non-zero
size, and (when `IMediaEngine::IsAvailable()`) an `FFmpegEngine::Probe()` round-trip —
before ever calling `SetResult()` (spec section 27, "a download is not complete simply
because yt-dlp exited successfully"). Any failure past step (2) triggers a best-effort
cleanup sweep of every file in the output directory whose name starts with the chosen
base name — safe specifically because that name was chosen NOT to collide with anything
that predates this job.

`QualityPreset` (`core/downloads/QualityPreset.h`) is the only quality vocabulary anything
above `engines/downloader` ever sees (`BEST`, `2160P`, `1440P`, `1080P`, `720P`, `480P`,
`AUDIO_ONLY`). `engines/downloader/YtDlpFormatSelector.h` is the one place a preset becomes
a concrete yt-dlp `-f` selector string — see `docs/decisions.md` for why that translation
lives in C++ rather than in `downloader.py`.

Video/audio merging is NOT reimplemented via `FFmpegEngine` — see `docs/decisions.md`
"Video/audio merge strategy" for why yt-dlp's own internal ffmpeg invocation is used
instead (pointed at the exact binary `engines/ffmpeg/FFmpegDiscovery` already resolved, so
there's still only one ffmpeg-discovery authority in the app).

The Python protocol gained a second command (`inspect`, alongside `download`) — see
`docs/protocols/downloader.md` for the full wire shape, including the richer
`DownloadMetadata`/`DownloadFormat` fields and the expanded error classification
(private/removed/geo-restricted/playlist/format-unavailable, on top of Phase 1's
network-failure detection).

### Known limitations (Phase 2)

- `inspectDownloadUrl` and `DownloadJob`'s internal re-inspect both run synchronously
  inside their caller (the IPC loop thread, or the job's worker thread respectively) —
  there's no async/streaming metadata delivery. This is fine for the vertical slice; a
  future phase could move `inspectDownloadUrl` onto a worker thread with a correlated
  response if blocking the IPC loop for a few seconds ever becomes a real problem.
- Pause/resume is not implemented for `DownloadJob` (`SupportsPause()` is `false`) — only
  cancel. yt-dlp has no clean "pause a download" primitive; a Phase 3+ implementation
  would need to stop and later resume via HTTP range requests, which is real design work,
  not a Phase 2 vertical-slice concern.
- Playlist URLs are explicitly rejected (`E_PLAYLIST_NOT_SUPPORTED`) rather than silently
  downloading only the first video — see spec section 30 ("prepare, don't fully
  implement").

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
implementation — `MockDownloadProvider` and `MockFileSystem` (both added in Phase 2)
round out the three mocks spec section 39 calls out by name for `DownloadJob` tests;
`IMediaEngine` and `IClock` are exercised via a scripted fake/nullptr rather than a
dedicated `Mock*` class, which was enough for what currently depends on them. No unit
test needs a real ffmpeg, a real network connection, or the real `%LOCALAPPDATA%` to pass
— see `docs/development.md`'s testing section for how to run the suites.

## What's scaffolded vs. working

See the root `README.md`'s "Phase 1 status" table for the authoritative, per-component
breakdown — do not assume a class exists just because its header does; several interfaces
in this document (`IImageEngine`, `IDocumentConverter`, `Convert`/`Compress` on
`IMediaEngine`) are declared but intentionally not implemented yet.
