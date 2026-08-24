# Technical roadmap

**Phase 1 — COMPLETED.** Foundation: Tauri/React shell, C++ core, job system, IPC,
FFmpeg discovery, Python downloader scaffold, five mockable interfaces, tests, docs.

**Phase 2 — COMPLETED.** The download vertical slice: real metadata inspection (with
full format list), quality selection, a real `DownloadJob` running on the existing
`JobManager`, real progress/speed/ETA, cancellation, output verification via `ffprobe`,
collision-safe filenames, and a functional (not final) downloader UI. See
`docs/phase-2.md` for the full report and `docs/decisions.md` for what was decided along
the way. See the root `README.md` for the authoritative working-vs-scaffolded table.

**Phase 5 — COMPLETED.** The unified media-job queue: downloads, conversions and
compressions in one queue with configurable concurrency, priorities, reordering,
dependencies (including a real download -> convert -> compress pipeline), bounded retries
with classification and backoff, durable versioned state with crash recovery, a queue UI,
and a substantially expanded test suite. This phase also built the conversion/compression
layer it depended on, which did not exist yet — see `docs/phase-5.md` and
`docs/decisions.md`.

Everything below is planned, not built.

## Downloads (remaining)
- Playlists: sequential download, preserved order, per-item numbering (Phase 2
  deliberately rejects playlist URLs rather than guessing — see `docs/decisions.md`)
- A live speed-over-time graph (the event architecture already supports periodic speed
  samples; only the chart itself is unbuilt)
- ~~Bounded automatic retries~~ — done in Phase 5, with classification and backoff
- ~~Configurable concurrent-download limits (N > 1)~~ — done in Phase 5; the limit is now
  a runtime setting enforced centrally across all job types
- Additional sites as yt-dlp supports them beyond YouTube: Vimeo, Streamable, Medal,
  others (architecture already supports this — `IDownloadProvider`/`YtDlpProvider` are
  not YouTube-specific — just not manually verified against those sites yet)
- Native output-folder picker (deferred — see `docs/decisions.md`)

## Video conversion
MP4 / MOV / MKV / WebM / GIF are implemented as of Phase 5 (`core/media/ProcessingOptions.h`
is the closed set). Remaining: per-codec control beyond the built-in recipes, hardware-
accelerated encoding, and frame-sequence output.

## Audio
MP3 / WAV / M4A / FLAC / Opus are implemented as of Phase 5. Remaining: per-codec tuning
beyond a bitrate, and audio-only extraction as a distinct operation (`ExtractAudio` on
`IMediaEngine` is still declared-but-unimplemented).

## Images
JPEG <-> PNG, PNG -> WebP, PNG -> AVIF, GIF -> frame sequence, frame sequence -> GIF.

## Compression
- ~~Quality-based video compression~~ and ~~resolution reduction~~ — done in Phase 5
- Target-size compression ("compress this 2GB video to ~500MB") — needs two-pass encoding
- Image compression
- Codec selection and hardware-accelerated encoding where available

## Documents / text
MD -> TXT, MD -> HTML, HTML -> TXT, HTML -> PDF, and related conversions.

## Batch processing
Multi-file and folder drops, recursive folder processing, preserving folder structure in
output.

## Queue
Built in Phase 5 — see `docs/phase-5.md`. Remaining:
- Resource-aware scheduling (a download and a 4K encode currently cost the same slot)
- True per-job pause, if a resumable-output strategy is ever adopted for yt-dlp/FFmpeg
- Sharing queue state between two running instances of the app

## Workflows
Phase 5 implements the dependency model underneath composable pipelines: jobs can depend on
other jobs, and a stage can consume the previous stage's output by naming it
(`inputFromJobId`), so Download -> Convert -> Compress works today. Remaining is the
user-facing part — a saved, reusable workflow definition, plus the "-> Move" style steps —
and `BatchJob`/`WorkflowJob` as job types in their own right.

## UI
The final home screen: dark-mode only, modern, minimal, premium, smooth, with two large
curved cards side by side — "Drag & drop a file to convert or compress" and "Paste a
media URL to download" — subtle borders/shadows, one strong accent color, no gradient-heavy
"AI SaaS" aesthetic, excellent typography, smooth micro-interactions. Phase 1 ships only a
bare developer console proving the IPC pipeline; this design work has not started.

## Explicitly out of scope, always
No accounts, no analytics/telemetry, no cloud processing or uploads, no artificial file
size limits (everything runs locally) — see spec section 24 and `docs/architecture.md`.
