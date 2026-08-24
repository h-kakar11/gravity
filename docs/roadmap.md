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

**Phase 6 — COMPLETED.** The application shell and design system: a dark, cohesive
five-screen product (Home, Download, Convert & Compress, Queue, Settings) replacing the
Phase 1-5 developer-console look, plus toasts, motion, an accessibility pass, and 18 new
frontend tests. See `docs/phase-6.md` for the full report.

**Phase 7 — COMPLETED (with real, documented limitations).** Distribution engineering:
CWD-independent resource resolution, FFmpeg/sidecar bundling *mechanism* (the actual FFmpeg
and Python+yt-dlp binaries were not sourced/bundled in this session — no Windows machine
available), NSIS packaging configuration, a real product icon, consistent Gravity branding
throughout (including the on-disk `%LOCALAPPDATA%\Gravity\` data directory, renamed from
`MediaTool`), and a single version source of truth. See `docs/phase-7.md` for exactly what
was and wasn't verified.

**Phase 8 — COMPLETED.** Adversarial hardening: real IPC fuzzing against the live binary
(found and fixed a crash from a single malformed byte), a real audit of process spawning
(found and fixed silently-broken stderr capture and a cancellation hang, both dating to
Phase 1), scheduler stress testing at volume, and an investigated (not dismissed)
environmental finding. See `docs/phase-8.md`.

**Phase 9 — COMPLETED. V1 feature freeze.** A product audit against every prior phase,
one genuine v1 gap closed (an About panel with real Gravity/FFmpeg/yt-dlp version info),
and every deferred capability (playlists, native pickers, GIF, hardware encoding,
target-size compression) explicitly documented with its reason rather than left ambiguous.
See `docs/phase-9.md` for the audit and `docs/v1-feature-freeze.md` for the frozen scope.
**No new major features from this point forward** — only bug fixes, security fixes,
performance fixes, and release blockers.

Everything below is planned for a future major version, not v1.

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
Built in Phase 6 — see `docs/phase-6.md` for the design system, shell, and per-screen
report. Remaining, deliberately deferred:
- A settings.json-backed appearance toggle, if a light theme is ever a real product
  requirement (Phase 6 ships dark-only by design, not as a placeholder for a toggle)
- Wiring the currently-inert `Settings.h` fields (`defaultOutputDirectory`,
  `defaultQuality`, `filenameTemplate`, `hardwareAccelerationEnabled`, and others — see
  `docs/phase-6.md` "Settings scope") to real backend behavior, at which point they belong
  in the Settings page
- A native file/folder picker (still a plain text field; see `docs/decisions.md`)
- Windows high-DPI/scaling verification (Phase 6 verified layout only on a Linux virtual
  display — see `docs/phase-6.md` "Known limitations")

## Explicitly out of scope, always
No accounts, no analytics/telemetry, no cloud processing or uploads, no artificial file
size limits (everything runs locally) — see spec section 24 and `docs/architecture.md`.
