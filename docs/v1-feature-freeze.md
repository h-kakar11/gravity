# Gravity v1 — Feature Freeze

This document is the line. Everything below "Included" is what Gravity v1 does. Everything
below "Explicitly excluded" is a real, considered decision, not an oversight — each entry
says why. After this document, per Phase 9's own charter: **no new major features.** Bug
fixes, security fixes, performance fixes, and release blockers only.

## Included in v1

**Download**
- Paste a URL, inspect real metadata (title, uploader, duration, thumbnail, available
  formats) before committing to anything
- Choose quality from a curated preset list (`BEST` down to `480P`, plus `AUDIO_ONLY`) —
  never a raw format string
- Real progress (percentage, speed, ETA, bytes transferred), cancellable mid-download
- Output verified (`ffprobe`) before a job is ever marked complete
- Collision-safe filenames, including under real concurrency (three simultaneous downloads
  of the same title produce three distinct files, not a race)

**Convert & Compress**
- Convert a local file between a curated set of containers: MP4, MKV, WEBM, MOV (video);
  MP3, WAV, M4A, FLAC, OPUS (audio)
- Compress with three plain-language presets (Smaller file / Balanced / High quality), an
  optional max-height downscale
- A download → convert → compress pipeline, built by declaring which job's output the next
  stage reads from — never by the frontend guessing a filename or polling for completion

**Queue**
- One unified queue for every job type — no separate download/conversion/compression queues
- Configurable concurrency, applied live
- Priority (Low/Normal/High) with bounded fairness aging so a low-priority job isn't starved
  forever
- Manual reordering of pending work
- Pause/resume the whole queue; cancel an individual job
- Bounded automatic retry with classification (transient vs. permanent) and exponential
  backoff; manual retry for anything, including a permanently-failed job
- Durable, versioned, atomically-written state that survives a crash or restart —
  corruption is quarantined for diagnosis, never silently destroyed or blindly trusted
- Bounded history so a long-running queue stays a bounded structure, not unbounded growth

**Application shell**
- Five real screens: Home, Download, Convert & Compress, Queue, Settings — a dark,
  cohesive design system throughout, not a developer console
- Toast notifications for outcomes that matter (completed/failed/cancelled/retry-scheduled/
  queue-restored), never for routine progress
- Keyboard-navigable, with visible focus and non-color-only status indicators

**Settings & About**
- Only settings with a demonstrated real backend effect are editable: notifications on/off,
  the FFmpeg path override, with live queue concurrency shown (controlled from the Queue
  screen itself)
- Basic product information: Gravity's version, FFmpeg's version, yt-dlp's version — no
  developer debugging information exposed by default

**Packaging**
- Resource resolution independent of current working directory or launch method (Start
  Menu, Desktop shortcut, direct launch)
- User data (settings, queue state, logs) kept separate from application files, surviving
  an upgrade or reinstall
- A single version number across the C++/Rust/frontend manifests, mechanically checked
  (`scripts/check_versions.py`)

## Explicitly excluded from v1

| Feature | Why |
|---|---|
| **Playlist support** | The single-URL journey is complete and solid; a real playlist implementation (inspection, item selection, batch duplicate detection, "don't blindly enqueue hundreds of jobs" awareness) is a substantial feature in its own right, not a gap-fill. The architecture supports adding it later without a redesign. |
| **Native file/folder picker** | Cosmetic, not functional — every path field works correctly as a text field today. Deferred to be done consistently across every path field at once, not piecemeal. |
| **GIF conversion** | The spec is explicit: do not fake GIF support through a poor one-pass pipeline. Real support needs two-pass palette generation, not yet built. Not in the current target-format set. |
| **Hardware encoding (NVENC/QSV/AMF)** | Detection scaffolding has existed since Phase 1 (`availableEncoders`, always empty by design); the actual encode path does not exist, and this environment has no GPU or Windows machine to build or verify it responsibly. |
| **Target-size compression** ("~500MB") | Needs real iterative/two-pass bitrate calculation to mean what it says; a CRF preset pretending to hit a target size would be misleading. The existing quality-preset compression is the honest v1 answer. |
| **Accounts, telemetry, cloud processing** | Out of scope by design since Phase 1 — Gravity is local-first and stays that way. |

## Known limitations carried into v1

These are not scope decisions — they're gaps in *verification*, tracked so they aren't
mistaken for "done":

- **No actual Windows installer has been built or tested** (`docs/phase-7.md`) — this
  development/CI environment has no Windows machine or NSIS toolchain. The packaging
  configuration and resource-resolution code are complete and reviewed; the artifact itself
  is not produced here.
- **FFmpeg and a Python+yt-dlp runtime are not actually bundled into the installer
  resources** (`docs/phase-7.md`) — the mechanism and the manual steps to populate them are
  documented; the binaries themselves need a release engineer with a Windows machine.
- **An intermittent sandbox-environment slowdown was investigated, not eliminated**
  (`docs/phase-8.md`) — confirmed to predate this project's own changes and most consistent
  with CPU-scheduling variance in this specific shared session, not a product defect, but
  not reproduced or ruled out on real target hardware.

## Future candidates (post-v1, if pursued)

In roughly the order they'd become worth doing, per `docs/roadmap.md`:

1. Playlist support (the largest deferred item, and the most requested kind of feature for
   a tool like this)
2. Native file/folder pickers, done consistently across every path field
3. Additional yt-dlp-supported sites beyond YouTube (architecture already supports this —
   not manually verified against other sites)
4. GIF two-pass support
5. Hardware-accelerated encoding, if a real Windows+GPU test environment becomes available
6. Target-size compression via iterative bitrate calculation
7. Image and document conversion (`IImageEngine`/`IDocumentConverter` — interfaces exist,
   deliberately unimplemented since Phase 1)
8. Batch/folder processing (`BatchJob` — named in the type system, not implemented)
9. Saved, reusable workflow definitions beyond the current ad hoc dependency pipelines

None of these are committed to. This list exists so a real future decision starts from an
accurate picture of what's already deferred and why, not from scratch.
