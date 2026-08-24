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
 +-- DownloadJob         Phase 2 — real, see "The download pipeline" below
 +-- MediaProcessingJob  Phase 5 — shared lifecycle for the two local-file operations
 |    +-- ConversionJob   Phase 5 — real
 |    \-- CompressionJob  Phase 5 — real
 +-- BatchJob            scaffolded (named in JobType, not implemented)
 +-- WorkflowJob         scaffolded (named in JobType, not implemented)
 \-- TestJob              Phase 1 — synthetic, proves the pipeline end-to-end
```

`JobStateMachine` (`core/jobs/JobStateMachine.h`) is the only thing allowed to decide
whether a state transition is valid — the authoritative table lives in that header, and
`docs/ipc-contract.md` mirrors it for the wire.

## The queue (Phase 5)

There is exactly **one** queue. Downloads, conversions and compressions are all ordinary
jobs in it; there is no download queue, conversion queue or compression queue, and no job
type has a scheduling path of its own.

`JobManager` is that queue — the Phase 1 class evolved, not a second orchestrator layered
beside it. Its Phase 1 public surface is unchanged; what changed is the machinery
underneath, plus the queue operations added alongside. Responsibilities split cleanly:

```
core/queue/SchedulerCore   DECIDES    ordering, priority, concurrency admission,
                                      dependency gating, retry timing.
                                      Pure: no threads, no locks, no clock, no Job objects.

core/jobs/JobManager       EXECUTES   owns Job objects, runs them on worker threads,
                                      keeps records in step, emits events, persists.
```

That split is why the scheduling rules are testable as ordinary deterministic function
calls with no sleeps: `SchedulerCore` takes the current time as a parameter rather than
reading a clock.

One scheduler thread dispatches; one worker thread runs each job. **Concurrency is enforced
by the scheduler's admission decision rather than by a thread pool's size**, which is what
lets the limit change at runtime and what makes it a cap on real running processes rather
than on job objects. Two locking rules keep it deadlock-free and are stated at the top of
`core/jobs/JobManager.h`: the manager's mutex is never held while calling into a `Job`
(whose methods fire callbacks that re-enter the manager), and never while joining a worker
(which may be blocked entering the manager).

Supporting pieces, all under `core/queue/`: `RetryClassifier` (transient vs. permanent),
`BackoffPolicy` (bounded exponential, no jitter), `QueuePersistence` (versioned JSON via
`AtomicWriter`, corruption recovery, restart recovery), `JobRecord` (the scheduling
metadata that is *not* on a `Job`, and the unit that gets persisted), `QueueTypes`
(priority, run state, statistics, duplicate keys).

See `docs/phase-5.md` for the full design, including the retry classification table, the
recovery policy, and why per-job pause is unsupported for media jobs.

## The event system

`EventBus` (`core/events/EventBus.h`) is a synchronous pub/sub bus. `JobManager` and the
logger publish structured `Event`s (`core/events/Event.h`); the `mediatool-core`
executable's IPC loop subscribes and turns them into NDJSON lines on stdout. The frontend
never parses human-readable log text to infer state (spec section 8) — every state change
and progress update is a typed event with a fixed JSON shape.

Two ordering guarantees were added in Phase 5. Every event line carries a monotonic `seq`,
**stamped as the line is written**, under the same lock that serializes stdout — so
sequence order and wire order are the same thing. (Stamping it where the event is
constructed does not work: several threads publish concurrently, so increasing numbers
reach the wire out of order. That was a real bug, caught end-to-end.) Sequence therefore
lives in the IPC layer rather than on `Event`: ordering is a property of the channel, not
of the event. Separately, each job carries a `revision` that increments on every durable
change, which catches the case where two jobs' events interleave and a newer `seq` does not
mean newer information about *that* job.

Progress events are throttled per job and coalesced, so a chatty encode produces tens of
events rather than thousands.

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
  cancel. yt-dlp has no clean "pause a download" primitive; an implementation would need to
  stop and later resume via HTTP range requests, which is real design work. Phase 5 reached
  the same conclusion for conversion and compression, and made the refusal explicit rather
  than showing a "Paused" state over a still-running process — see `docs/phase-5.md`
  → "Pause semantics".
- Playlist URLs are explicitly rejected (`E_PLAYLIST_NOT_SUPPORTED`) rather than silently
  downloading only the first video — see spec section 30 ("prepare, don't fully
  implement").

## Filesystem, temp files, and atomic output

`core/filesystem/LocalFileSystem` implements `IFileSystem` on top of `std::filesystem`
(never manual path string concatenation, spec section 11). `TempDirectory` gives each job
an isolated working directory under `%LOCALAPPDATA%\Gravity\temp\job-<id>\`; `AtomicOutput`
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

## Filename collisions under concurrency

Deduplication answers "is this name free?" by looking at the disk. Two jobs that ask before
either has written both get "yes" — unreachable when only one job ran at a time, routine
once Phase 5 runs several. The observed result was three downloads of the same title all
reporting success with one file on disk.

So jobs **reserve** an output name through `filesystem::OutputNameRegistry` rather than
merely deduplicating one. The lock spans choosing and recording, so two callers cannot
settle on the same candidate, and the reservation releases on scope exit so a cancelled or
failed job cannot leak a claim. It is process-wide because the resource it guards — the set
of names this application is about to write — is itself process-wide.

## Frontend architecture (Phase 6)

`app/frontend/src/App.tsx` owns exactly one `useQueue()` instance and passes it down —
every screen that needs job data reads that same store rather than opening its own
subscription (Phase 6 removed a second, duplicate polling hook that `DownloaderPage` and
`DevConsole` each had). `AppShell.tsx` renders the five real navigation destinations around
whichever page is active; `state/queueReducer.ts` (unchanged from Phase 5) stays the single
source of truth for what a job's state is. `state/useQueueNotifications.ts` watches that
same store for transitions worth a toast, entirely outside the reducer, so "what the store
contains" and "what the user is told about it" stay two separate, independently testable
concerns. `styles/theme.css` and `styles/components.css` are the design system: CSS custom
properties for every color/spacing/radius/motion value, consumed by class name rather than
scattered inline styles. See `docs/phase-6.md` for the full design system, per-screen
report, and accessibility/verification notes.

## What's scaffolded vs. working

See the root `README.md`'s status table for the authoritative, per-component breakdown — do
not assume a class exists just because its header does; several interfaces in this document
(`IImageEngine`, `IDocumentConverter`, `ExtractAudio`/`ExtractFrames` on `IMediaEngine`) are
declared but intentionally not implemented yet, and say so when called.

## Product scope (Phase 9)

As of Phase 9, Gravity's feature set is frozen for v1 — see
`docs/v1-feature-freeze.md` for exactly what's included, what's deliberately deferred (and
why), and what a future version might add. The architecture above was deliberately kept
open to several of those deferrals without a redesign: `IDownloadProvider`'s per-item
`createJob` path could support playlist fan-out without new plumbing; `IImageEngine`/
`IDocumentConverter` already exist as interfaces for exactly the day image/document
conversion becomes real; the queue's dependency model already supports an arbitrary
pipeline shape, not just the three-stage download→convert→compress case it's used for
today.
