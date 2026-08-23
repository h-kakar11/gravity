# Technical roadmap

Phase 1 (this repository, current) builds the foundation only — see the root
`README.md` for exactly what is implemented vs. scaffolded today. Everything below is
planned, not built.

## Downloads
- YouTube: single video, highest available quality, separate video/audio stream merging
- Playlists: sequential download, preserved order, per-item numbering
- Custom output directory, automatic filenames from video titles
- Live download speed, ETA, and a speed-over-time graph
- Retry handling, cancellation, future concurrent-download limits (N > 1)
- Additional sites as yt-dlp supports them: Vimeo, Streamable, Medal, others

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
