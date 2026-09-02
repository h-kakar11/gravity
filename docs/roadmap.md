# Technical roadmap

**Phase 1 — COMPLETED.** Foundation: Tauri/React shell, C++ core, job system, IPC,
FFmpeg discovery, Python downloader scaffold, five mockable interfaces, tests, docs.

**Phase 2 — COMPLETED.** The download vertical slice: real metadata inspection (with
full format list), quality selection, a real `DownloadJob` running on the existing
`JobManager`, real progress/speed/ETA, cancellation, output verification via `ffprobe`,
collision-safe filenames, and a functional (not final) downloader UI. See
`docs/phase-2.md` for the full report and `docs/decisions.md` for what was decided along
the way. See the root `README.md` for the authoritative working-vs-scaffolded table.

Everything below is planned, not built.

## Downloads (remaining)
- ~~Playlists: sequential download, preserved order, per-item numbering~~ — **shipped**
  (issue #41; see `docs/decisions.md`). Remaining playlist work, if wanted: a single
  queue row that groups a playlist's jobs instead of N independent rows, and resuming a
  partially-downloaded playlist by skipping entries already on disk.
- A live speed-over-time graph (the event architecture already supports periodic speed
  samples; only the chart itself is unbuilt)
- Bounded automatic retries (`RetryJob` already exists on `JobManager`; `DownloadJob`
  doesn't yet wire a retry policy into it)
- Configurable concurrent-download limits (N > 1) — `JobManager`'s worker pool size is
  already configurable, just not yet exposed for downloads specifically
- Additional sites as yt-dlp supports them beyond YouTube: Vimeo, Streamable, Medal,
  others (architecture already supports this — `IDownloadProvider`/`YtDlpProvider` are
  not YouTube-specific — just not manually verified against those sites yet)
- Native output-folder picker (deferred — see `docs/decisions.md`)

## Video conversion
MP4 <-> MOV, MP4 -> WebM, video -> GIF, MKV -> MP4, and similar container/codec conversions.

## Audio
MP4 -> MP3, MP4 -> WAV, FLAC -> MP3, M4A -> MP3, and similar.

## Images
JPEG <-> PNG, PNG -> WebP, PNG -> AVIF, GIF -> frame sequence, frame sequence -> GIF.

## Compression
- Image and video compression
- Quality-based and target-size-based compression ("compress this 2GB video to ~500MB")
- Resolution reduction, codec selection, hardware-accelerated encoding where available

## Documents / text
MD -> TXT, MD -> HTML, HTML -> TXT, HTML -> PDF, and related conversions.

## Batch processing
Multi-file and folder drops, recursive folder processing, preserving folder structure in
output.

## Queue
Pause, resume, cancel, retry, priority, and configurable concurrency across job types.

## Workflows
Composable multi-step pipelines, e.g. Download -> Convert -> Compress -> Move. The Phase 1
`Job`/`JobManager` design (see `docs/architecture.md`) is deliberately built so this
doesn't require a redesign — `WorkflowJob` and `BatchJob` are already named in the job
type hierarchy, just not yet implemented.

## UI
The final home screen: dark-mode only, modern, minimal, premium, smooth, with two large
curved cards side by side — "Drag & drop a file to convert or compress" and "Paste a
media URL to download" — subtle borders/shadows, one strong accent color, no gradient-heavy
"AI SaaS" aesthetic, excellent typography, smooth micro-interactions. Phase 1 ships only a
bare developer console proving the IPC pipeline; this design work has not started.

## Explicitly out of scope, always
No accounts, no analytics/telemetry, no cloud processing or uploads, no artificial file
size limits (everything runs locally) — see spec section 24 and `docs/architecture.md`.
