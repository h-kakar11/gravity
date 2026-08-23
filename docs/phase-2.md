# Phase 2 — Download Engine Vertical Slice

## Summary

Phase 2 turned the download side of MediaTool from a scaffold into a real, working
vertical slice: enter a URL, inspect real metadata (title, uploader, duration, thumbnail,
every available format), pick a quality, start a real download through the existing
`Job`/`JobManager` architecture, watch real progress/speed/ETA, cancel mid-flight if
needed, and get a verified, playable output file. Nothing in this flow is simulated —
every piece was verified against real, live YouTube videos with real network access (see
"Manual test results" below).

## Repository

`https://github.com/h-kakar11/gravity` (private), branch `master`. The Phase 1 commit
(`21c8be0`) is the repository's first commit — pushed as-is, no history rewritten. See
`docs/decisions.md` for why a new repo was created rather than continuing local-only.

## Architecture

```
React (DownloaderPage) -> Tauri -> C++ core -> DownloadJob -> YtDlpProvider -> Python -> yt-dlp -> ffmpeg
```

Full detail in `docs/architecture.md`'s "The download pipeline (Phase 2)" section and
`docs/protocols/downloader.md`. In short:

- `inspectDownloadUrl(url)` is a direct IPC command (not a Job) returning full
  `DownloadMetadata` including every selectable `DownloadFormat`.
- `createJob({type: "DOWNLOAD", ...})` creates a real `DownloadJob` on the existing
  `JobManager` — no separate downloader queue.
- `DownloadJob` re-inspects the URL itself, sanitizes and de-duplicates the output
  filename (`FilenameSanitizer::DeduplicateBaseName`, checked against any extension),
  drives the real download through `IDownloadProvider`, and verifies the result via
  `IFileSystem`/`ffprobe` before ever reporting success.
- `QualityPreset` (`BEST`/`2160P`/`1440P`/`1080P`/`720P`/`480P`/`AUDIO_ONLY`) is the only
  quality vocabulary anything outside `engines/downloader` ever sees;
  `YtDlpFormatSelector` is the one place it becomes a yt-dlp `-f` string.
- Video/audio merging uses yt-dlp's own internal ffmpeg invocation, pointed at the exact
  binary `engines/ffmpeg/FFmpegDiscovery` already resolved — `FFmpegEngine::Convert()`
  remains unimplemented, unchanged from Phase 1 (conversion is explicitly out of scope,
  spec sections 14/49).

## Implementation

**New:** `core/downloads/QualityPreset.{h,cpp}`, `core/downloads/MockDownloadProvider.{h,cpp}`,
`core/filesystem/MockFileSystem.{h,cpp}`, `core/jobs/DownloadJob.{h,cpp}`,
`engines/downloader/YtDlpFormatSelector.{h,cpp}`, `app/frontend/src/pages/DownloaderPage.tsx`,
`app/frontend/src/types/download.ts`, `app/frontend/src/utils/errors.ts`,
`docs/decisions.md`, `docs/protocols/downloader.md`, this file.

**Extended:** `core/downloads/IDownloadProvider.h` (added `Inspect()`, `DownloadFormat`,
richer `DownloadMetadata`), `core/filesystem/IFileSystem.h`/`LocalFileSystem` (added
`ListDirectory`), `core/filesystem/FilenameSanitizer` (added `DeduplicateBaseName`),
`engines/downloader/YtDlpProvider` (added `Inspect()`, a shared `RunPythonCommand` helper,
`ffmpegLocation` forwarding), `python/downloader/downloader.py` (added the `inspect`
command, richer error classification, `noplaylist`/`extract_flat` fast-path), `app/core/main.cpp`
(wired `inspectDownloadUrl` and `createJob{DOWNLOAD}`), `app/desktop/src-tauri/src/lib.rs`
(added `open_containing_folder`), the frontend IPC types/`coreClient.ts`/`App.tsx`.

**Fixed (found via integration testing, not written intentionally in Phase 1):**
`core/logging/Logger.cpp`'s console sink was writing to stdout — the exact channel
reserved for the NDJSON protocol — corrupting the very first line of every IPC session.
Moved to stderr. See `docs/decisions.md`.

## Working

Verified by actually running the code against real infrastructure, not just reading it:

- Real `inspectDownloadUrl` against live YouTube videos: correct title, uploader,
  duration, thumbnail, extractor, and a full format list (40 formats for a real video,
  correct `hasVideo`/`hasAudio`/resolution/codec/bitrate/filesize per format).
- Real `createJob{DOWNLOAD}` → real download → real progress events (percentage,
  processedBytes/totalBytes, speedBytesPerSecond, etaSeconds) → real completion, for both
  an audio-only download (~3.4 MB) and a 480p video+audio download that exercised yt-dlp's
  real ffmpeg merge step (`[Merger] Merging formats into "..."` in yt-dlp's own log output).
- Real output verification: the completed job's result includes an `ffprobe`-verified
  `FileInfo` (correct duration, codec, bitrate, resolution for the video case).
- Real filename collision handling: downloading the same video twice produced
  `"...(1).webm"` the second time, not an overwrite — `DeduplicateBaseName` working
  against real directory contents, not just mocked ones.
- Real cancellation: cancelling a real, in-flight 1080p download mid-stream (at ~14%)
  correctly transitioned the job to `CANCELLED` and left no partial (`.part`) artifacts
  behind on disk — `CleanupArtifacts()` verified against a real yt-dlp partial-download
  artifact, not a simulated one.
- Real input validation: an obviously-invalid URL, a non-http(s) scheme, an empty output
  directory, and a real playlist URL were all rejected cleanly with the correct
  `ErrorInfo` (`E_INVALID_DOWNLOAD_URL`, `E_INVALID_OUTPUT_DIRECTORY`,
  `E_PLAYLIST_NOT_SUPPORTED`) rather than hanging, crashing, or partially succeeding.
- Both `mediatool-desktop.exe` and `mediatool-core.exe` launch and run together via
  `npm run tauri dev` (same bar Phase 1 used) — confirmed via process list.
- Frontend: `tsc --noEmit` and `vite build` both pass with the new `DownloaderPage`,
  types, and `coreClient` additions.
- Rust: a full `cargo build` (not just `cargo check`, which would miss a linker-only
  failure the way Phase 1's `cdylib` bug was originally missed) succeeds.
- 146/146 GoogleTest cases pass (123 from Phase 1 + 23 new: `QualityPreset`,
  `YtDlpFormatSelector`, `DownloadJob` x7, `YtDlpProvider` Inspect() x4, `DeduplicateBaseName`
  x3, `ListDirectory` x2, plus a Phase 1 test updated for the new wire params).
- 24/24 Python `unittest` cases pass (6 from Phase 1's `--selftest` suite + 18 new
  protocol tests), all without touching the network.

## Scaffolded / not done

- **Pause/resume for downloads.** `DownloadJob::SupportsPause()` is `false` (inherited
  default) — yt-dlp has no clean pause primitive; a real implementation needs HTTP range
  requests, which is Phase 3+ work, not this vertical slice.
- **Playlists.** Deliberately rejected (`E_PLAYLIST_NOT_SUPPORTED`), not partially
  implemented, per spec section 30 ("prepare, don't fully implement").
- **Bounded automatic retries.** `JobManager::RetryJob()` exists and works (Phase 1); no
  policy currently triggers it automatically for a failed `DownloadJob`.
- **A live speed-over-time graph.** The event stream already carries periodic speed
  samples (spec section 20); no chart consumes them yet.
- **Native output-folder picker.** A plain text input is used instead — see
  `docs/decisions.md` for why (MinGW/Tauri plugin dependency-graph risk).
- **Sites other than YouTube.** `YtDlpProvider`/`IDownloadProvider` are not
  YouTube-specific by construction, but only YouTube was manually verified this phase
  (spec section 32: don't claim broader support than what's actually been tested).
- **Conversion/compression.** Untouched, as instructed (spec sections 14, 49).

## Testing

- **C++:** `ctest --preset windows-mingw-debug --output-on-failure` — 146/146 passing.
  New coverage: `DownloadJob` (success, filename dedup, download failure + cleanup,
  missing/empty output, inspect failure/cancellation, playlist rejection), `YtDlpProvider`
  (`Inspect()` success/error/cancellation/no-result, `ffmpegLocation` forwarding),
  `YtDlpFormatSelector`, `QualityPreset`, `FilenameSanitizer::DeduplicateBaseName`,
  `LocalFileSystem::ListDirectory`. Mocks used throughout: `MockDownloadProvider`,
  `MockFileSystem` (both new this phase), `MockProcessRunner` (Phase 1) — no test needs
  real yt-dlp, ffmpeg, or a network connection.
- **Python:** `python -m unittest discover -s tests/python` — 24/24 passing, all
  network-free (pure-function tests for `format_entry`/`build_metadata_payload`/
  `classify_exception`/`sanitize_filename`, plus subprocess protocol tests for malformed
  input, unsupported commands, and the "yt_dlp not installed" path).
- **Manual (real network, not automated — spec section 41):** see "Manual test results"
  below for what was actually run, and `docs/protocols/downloader.md` for the repeatable
  procedure.

## Manual test results

Run against `https://www.youtube.com/watch?v=dQw4w9WgXcQ` (chosen for stability — see
`docs/protocols/downloader.md`), through the real `mediatool-core.exe` NDJSON IPC loop:

| Scenario | Result |
|---|---|
| `inspectDownloadUrl` | PASS — title, uploader, duration, thumbnail, 40 real formats returned |
| `createJob{DOWNLOAD, quality: AUDIO_ONLY}` | PASS — QUEUED → STARTING → RUNNING → COMPLETED, real 3.4 MB `.webm` audio file, `ffprobe`-verified (opus, 213s duration) |
| `createJob{DOWNLOAD, quality: 480P}` | PASS — real video+audio download, real yt-dlp/ffmpeg merge, second run correctly renamed to `"...(1).webm"` to avoid overwriting the first, `ffprobe`-verified (480p, 25fps, opus audio) |
| Invalid URL (`"not a url at all"`) via `inspectDownloadUrl` | PASS — clean `E_INVALID_DOWNLOAD_URL` |
| Non-http(s) URL (`ftp://...`) via `createJob` | PASS — clean `E_INVALID_DOWNLOAD_URL` |
| Empty output directory | PASS — clean `E_INVALID_OUTPUT_DIRECTORY` |
| Real playlist URL | PASS — clean `E_PLAYLIST_NOT_SUPPORTED`, fast (flat extraction, no full playlist walk) |
| Cancel a real, in-flight 1080p download | PASS — CANCELLED at ~14%, no partial file left on disk |
| Full Tauri app launch (`npm run tauri dev`) | PASS — `mediatool-desktop.exe` and `mediatool-core.exe` both running simultaneously (process list) |

**Not captured this session:** an actual click-through of the running Tauri window (typing
a URL into the real UI, clicking Inspect/Download/Cancel/Open Folder with a mouse). The
window launched successfully, but this session's screen-automation tooling hit a parameter
-encoding error on every call that needed coordinate/array arguments, independent of the
application. Given every backend call `DownloaderPage.tsx` makes was separately verified
for real (above), and the Rust bridge code that connects them is unchanged
request/response passthrough already proven in Phase 1, this is recorded as an honest gap
in this session's verification method, not a known defect in the product.

## Commands

```bash
# C++ core (from repo root)
cmake --build --preset windows-mingw-debug
ctest --preset windows-mingw-debug --output-on-failure

# Python tests (ambient interpreter, not the venv)
python -m unittest discover -s tests/python

# Frontend
cd app/frontend && npx tsc --noEmit && npx vite build

# Rust (full build, not just `cargo check` -- see docs/development.md for why)
cd app/desktop/src-tauri && cargo build

# Full desktop app
cd app/desktop && npm run tauri dev
# If it panics finding mediatool-core, see docs/development.md's MEDIATOOL_CORE_PATH note.
```

## Known issues

1. **`Logger`'s console sink wrote to stdout instead of stderr** (found and fixed this
   phase — see `docs/decisions.md`). Any code path that logs during `RunIpcLoop()` before
   this fix would corrupt the NDJSON stream.
2. **The dev-mode sidecar path guess in `core_bridge.rs` is CLI-invocation-dependent.**
   The same "run from `app/desktop`" assumption that worked earlier in this project broke
   partway through this session when `cargo run`'s working directory turned out to be
   `app/desktop/src-tauri` instead — a one-directory-level difference in a relative-path
   guess that the code's own comments already flagged as fragile. Workaround
   (`MEDIATOOL_CORE_PATH` etc.) documented in `docs/development.md`; a real packaging
   phase resolving the sidecar relative to the app resource directory instead of CWD
   removes this class of bug entirely.
3. **`inspectDownloadUrl` and `DownloadJob`'s internal re-inspect block their caller's
   thread** (the IPC loop thread, or the job's worker thread) for the duration of the
   network probe. Fine for a vertical slice; see `docs/architecture.md`'s "Known
   limitations" if this needs to become async later.
4. **A `.webm` audio-only file is categorized `"VIDEO"` in `FileInfo`.**
   `LocalFileSystem`'s extension→category map (Phase 1) treats `.webm` as video
   unconditionally; it doesn't consult the actual stream content the way `ffprobe`'s
   enrichment does for duration/codec/etc. Harmless for Phase 2's purposes (the enriched
   fields are still correct) but worth a real fix if file-category-driven UI logic is
   ever added.
5. **The GUI click-through gap** described in "Manual test results" above.

## Architectural decisions

See `docs/decisions.md`'s "Phase 2" section for the full list with context/options/reasoning:
repository creation, re-inspecting inside `DownloadJob` instead of trusting frontend-supplied
metadata, where format-selector translation lives, the video/audio merge strategy, the new
`DeduplicateBaseName` primitive and its cleanup-sweep counterpart, deferring the native
folder picker, the `open_containing_folder` implementation, and the Logger stdout→stderr fix.

## Recommended Phase 3

In priority order, given what Phase 2 revealed:

1. **Playlist support**, building on `DownloadJob`/`IDownloadProvider` as they already
   exist — the architecture was explicitly kept ready for this (spec section 30).
2. **A universal job queue UI** (concurrency > 1 is already supported by `JobManager`;
   only the UI and a per-download-type default need building).
3. **Bounded retry policy** wiring `JobManager::RetryJob()` into `DownloadJob` failures.
4. **Sidecar path resolution for real packaging** (resolve relative to the app resource
   directory, not CWD) — removes known issue #2 above permanently, and is needed for
   distribution regardless.
5. Only after the above: the first conversion/compression vertical slice (spec sections
   14/49 — explicitly deferred twice now, for good reason).

## Open architectural decisions carried into Phase 3

- Whether `inspectDownloadUrl`/`Inspect()` need to move off the IPC-loop/worker thread
  onto something that can run concurrently with other commands (currently fine; only
  matters once real users report the "one download blocks other UI actions" experience).
- Whether the native folder picker's MinGW/Tauri-plugin risk is worth revisiting once
  more of the UI work (`docs/roadmap.md` "UI") begins, or whether a different plugin
  (lighter dependency graph) becomes available.

---

Per the original instruction: **Phase 2 is complete. Do not start Phase 3 automatically.**
