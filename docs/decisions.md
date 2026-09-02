# Architectural decisions

Concise log of choices made where more than one reasonable option existed, per spec
section 47. Newest entries at the bottom of each phase's section.

## Phase 1

### Sidecar process over stdio NDJSON, not Rust<->C++ FFI
- **Context:** the Tauri (Rust) shell needs to talk to the C++ core.
- **Options considered:** (a) compile C++ as a static/shared lib, bind via `cxx`/`bindgen`;
  (b) a standalone `mediatool-core.exe` sidecar over stdio NDJSON.
- **Choice:** (b).
- **Reason:** matches the "controlled process abstraction" philosophy already required
  for FFmpeg/yt-dlp; avoids Rust/C++ ABI and cross-toolchain concerns (C++ built with
  MinGW, Rust with its own toolchain); keeps the C++ core independently runnable/testable
  via `--selftest` with no Tauri/Rust in the loop.
- **Consequences:** one process-spawn and a JSON-parsing hop per command — irrelevant for
  operations measured in seconds to minutes. See `docs/architecture.md`.

### MinGW-w64 GCC instead of MSVC
- **Context:** this session has no admin/UAC access, and full Visual Studio Build Tools
  needs an admin-elevated, multi-GB installer.
- **Options considered:** (a) MSVC via Visual Studio Build Tools; (b) MinGW-w64 GCC,
  installable per-user via winget.
- **Choice:** (b), for both the C++ core and the Rust toolchain (`stable-x86_64-pc-windows-gnu`).
- **Consequences:** hit a real limitation in Phase 1 — GNU `ld` assigns every exported
  cdylib symbol a 16-bit ordinal, which Tauri's 300+-crate dependency graph overflowed
  ("export ordinal too large"). Fixed by dropping `cdylib` from the Tauri crate's
  `crate-type` (only needed for mobile targets, irrelevant here). Documented in
  `app/desktop/src-tauri/Cargo.toml` and `docs/development.md`.

## Phase 2

### Repository: new GitHub repo, not a rename of the local-only Phase 1 repo
- **Context:** Phase 2's kickoff asked for "the new repository requested for this phase"
  without naming one or specifying a remote.
- **Options considered:** (a) keep `A:\Coding\gravity` local-only; (b) create a new GitHub
  repo and push the existing history into it, no duplication.
- **Choice:** (b) — asked the user directly rather than guessing a name/visibility.
  Created `https://github.com/h-kakar11/gravity` (private), pushed the existing `master`
  branch (commit `21c8be0` preserved as the repo's first commit, verified via
  `git ls-remote`).
- **Reason:** creating/pushing to a new GitHub repository is a visible, shared-state
  action (name and visibility are choices only the repo owner can make) — not something
  to infer silently.
- **Consequences:** the project and the repo are both named "gravity" per the user's
  explicit choice.

### `IDownloadProvider::Inspect()` re-run inside `DownloadJob`, not passed through `createJob`
- **Context:** the frontend already calls `inspectDownloadUrl` before the user clicks
  Download. `DownloadJob` also needs the video title to pick an output filename.
- **Options considered:** (a) have the frontend pass the already-fetched title into
  `createJob`'s params; (b) have `DownloadJob::Execute()` call `IDownloadProvider::Inspect()`
  again itself.
- **Choice:** (b).
- **Reason:** keeps `DownloadJob` fully self-contained and correct even if created without
  a prior `inspectDownloadUrl` call (spec section 3: "contain enough information to
  represent a download independently of the UI") — it doesn't trust frontend-supplied
  metadata as a source of truth for something the backend can verify itself.
- **Consequences:** one extra yt-dlp metadata fetch per download (~1-2s). Acceptable for a
  vertical slice; revisit only if that overhead is ever measured as a real problem.

### Format-selector translation lives in C++ (`YtDlpFormatSelector`), not in `downloader.py`
- **Context:** spec section 10 asks for a `QualityPreset` abstraction so nothing above the
  downloader engine knows yt-dlp selector syntax.
- **Options considered:** (a) keep Phase 1's `format_selector_for_quality()` in Python,
  have C++ just forward the preset name as a string; (b) move the preset->selector
  mapping into C++ (`engines/downloader/YtDlpFormatSelector.h`), send Python a
  ready-to-use `formatSelector` string.
- **Choice:** (b).
- **Reason:** spec section 10 says explicitly "The YtDlpProvider converts these
  application-level choices into yt-dlp-specific format selection" — Python becomes a
  dumb executor of whatever selector string it's handed, which also makes the mapping
  unit-testable in C++ without spawning a process (`tests/downloads/YtDlpFormatSelectorTest.cpp`).
- **Consequences:** the wire param renamed from `quality` (Phase 1, a raw string like
  `"best"`) to `formatSelector` (Phase 2, a concrete yt-dlp `-f` string) — a breaking
  change to an internal (C++<->Python only, never frontend-visible) protocol detail;
  documented in `docs/protocols/downloader.md`.

### Playlist URLs: decomposed into one DownloadJob per entry, run sequentially (issue #41)

*Supersedes the earlier decision to reject playlist URLs outright; that rationale is kept
below because the constraints it named are exactly what this design had to answer.*

- **Context:** `downloader.py`'s probe options and the `WithPlaylistIndex`/playlist-count
  scaffolding always existed, but nothing above them decomposed a playlist into per-video
  jobs -- a bare playlist URL was rejected with `E_PLAYLIST_NOT_SUPPORTED`. The earlier pass
  deferred this for want of product sign-off on the UX; that sign-off has now been given,
  with the specific answers recorded under "Choice" below.
- **Options considered:** (a) one job per entry, chained; (b) a single playlist job that
  loops internally; (c) keep rejecting.
- **Choice:** (a), with these product decisions:
  - **A combo URL (`watch?v=X&list=Y`) asks.** The page offers "just this video" or "the
    whole playlist" instead of guessing. Guessing "whole playlist" would turn an ordinary
    shared link into hundreds of downloads; guessing "just this video" would make a
    deliberately-pasted playlist link silently do the wrong thing.
  - **Destination is a named subfolder.** `<output>/<name>/`, with the name field
    pre-filled from `suggestPlaylistFolder` ("playlist #n" for the lowest unused n in that
    directory) and expected to be overwritten with the real playlist name. The default is
    collision-free so an unedited name can never merge two playlists; the field is editable
    because "playlist #3" is a poor thing to find on disk a month later.
  - **Entries download one at a time, in order,** via a `runAfter` chain (entry N+1 runs
    after entry N *finishes*, whatever the outcome).
  - **One quality/definition applies to the whole playlist,** chosen before download.
    Per-entry `formatId` is deliberately not offered: format ids are per-video and mean
    nothing across a list.
- **Reason:** one job per entry is what the existing machinery already supports properly.
  Per-item cancel/retry, progress, and the queue view all work unmodified, because each
  entry is an ordinary DownloadJob; a single looping job would have needed its own
  parallel implementation of all four. Numbering reuses `WithPlaylistIndex`, applied before
  the MAX_PATH truncation so the prefix is inside the length budget rather than pushing a
  name past it.
- **Why sequencing needed a new edge kind (`runAfter`).** The obvious way to chain entries
  was the existing `dependsOn`, and it is wrong here: `dependsOn` requires the predecessor
  to reach COMPLETED and *cancels* dependents when it doesn't, transitively. A playlist
  chained that way would lose every remaining entry the first time a video turned out to be
  private, deleted or geo-blocked -- routine in a long playlist, and the opposite of what
  "download all of them" means. `runAfter` waits only for a terminal state, so one bad video
  costs exactly one video. It is a separate edge kind rather than a flag on `dependsOn`
  because the workflow guarantee (download → convert → compress must not continue past a
  failure) is still exactly right for `dependsOn` and must not be weakened; the two are
  tested side by side in `SchedulerCoreTest`. Crash recovery remaps both edge kinds when it
  replays specs, so a recovered playlist resumes sequentially instead of starting every
  remaining entry at once.
- **Consequences:** fan-out is bounded (`_MAX_PLAYLIST_ENTRIES` = 500 in `downloader.py`,
  mirrored by `kMaxPlaylistEntries` in `main.cpp`); a longer playlist is truncated and the
  UI says so, rather than flooding the scheduler and the recovery store. `inspect` still
  fails with `E_PLAYLIST_NOT_SUPPORTED` for a playlist URL -- DownloadJob downloads exactly
  one video, so that guard stays -- and the frontend reads that code as "enumerate this
  instead", which is what makes playlist support work on any site rather than only on URLs
  whose shape we recognize. Entries yt-dlp reports as unavailable are dropped at enumeration
  and the remainder renumbered contiguously, so numbering has no gaps and no job is created
  that is certain to fail. A failed entry does not stop the rest — that is what `runAfter`
  buys, and `SchedulerCoreTest` pins it.

*Superseded rationale (kept for the record):* the original decision was to reject playlist
URLs, on the grounds that decomposing one touches job creation, queue behavior under #17's
priority ordering, per-item failure/retry UX, and progress reporting across N jobs -- a real
feature rather than a bug fix, which that audit pass had no sign-off to design unilaterally.
The `WithPlaylistIndex`/playlist-count fields were kept parked precisely so the numbering
scheme would not need re-deriving when support did ship, which is what happened.

### Video/audio merge strategy: yt-dlp's own ffmpeg invocation, pointed at our resolved path
- **Context:** "Best" quality and every resolution preset select separate video+audio
  streams that need merging into one container (spec section 16).
- **Options considered:** (a) reimplement a real merge in `FFmpegEngine::Convert()`
  ourselves; (b) let yt-dlp invoke ffmpeg internally via its own independent PATH
  discovery; (c) let yt-dlp invoke ffmpeg internally, but pass it the exact path
  `engines/ffmpeg/FFmpegDiscovery` already resolved.
- **Choice:** (c).
- **Reason:** (a) is out of scope — spec sections 14 and 49 explicitly forbid starting
  conversion/compression work in Phase 2, and `FFmpegEngine::Convert()` remains
  intentionally unimplemented (throws `UnsupportedFormat`, unchanged from Phase 1). (b)
  would create a second, redundant "how do we find ffmpeg" code path in the app. (c) keeps
  exactly one ffmpeg-discovery authority while still using a real, tested ffmpeg
  invocation for the actual merge.
- **Consequences:** `YtDlpProvider`'s constructor now takes an optional `ffmpegLocation`,
  resolved once at startup in `app/core/main.cpp` (not per-download, which also avoids the
  discovery process polluting `YtDlpProviderTest`'s scripted `FakeProcessRunner`). If
  ffmpeg isn't discovered, downloads needing a merge will fail with whatever error yt-dlp
  reports for a missing ffmpeg binary — audio-only or already-single-format downloads are
  unaffected.

### Collision-avoidance for an unknown final extension: `DeduplicateBaseName`, a new primitive
- **Context:** spec section 29 requires never silently overwriting an existing file, but
  the download pipeline doesn't know the final container extension until yt-dlp finishes
  (it depends on the merge/remux format).
- **Options considered:** (a) guess the likely extension (e.g. always assume `.mp4`) and
  reuse Phase 1's exact-path `DeduplicateFilename`; (b) add a new primitive that checks
  for a colliding base name against *any* extension, via a new `IFileSystem::ListDirectory()`.
- **Choice:** (b) — `filesystem::DeduplicateBaseName()`, matched by exact filename stem
  (not prefix) against `IFileSystem::ListDirectory()`'s results, to avoid false collisions
  like an unrelated "My Video (backup).mp4" matching base name "My Video".
- **Reason:** guessing the extension is fragile (wrong for audio-only downloads, webm
  vs. mp4 remuxing, etc.) and would occasionally let a real collision through undetected.
- **Consequences:** `IFileSystem` gained a fifth method (`ListDirectory`), implemented for
  `LocalFileSystem` (via `std::filesystem::directory_iterator`) and the new
  `MockFileSystem` (added this phase to round out spec section 39's three named mocks —
  process runner, filesystem, downloader provider — none of which had a filesystem mock
  before Phase 2).

### Post-failure cleanup: prefix match, not exact match, against `ListDirectory()`
- **Context:** a cancelled/failed download can leave yt-dlp artifacts behind (`.part`
  files, per-format intermediate files during a merge) that share the chosen base name
  but not its exact stem.
- **Choice:** `DownloadJob::CleanupArtifacts()` deletes anything in the output directory
  whose name starts with the job's `filenameBase` (`std::string::rfind(base, 0) == 0`),
  not just an exact match.
- **Reason:** this is safe specifically because `DeduplicateBaseName` chose that base name
  to not collide with anything that predates the job — so anything matching it after a
  failure was created by this run, regardless of its exact suffix.
- **Consequences:** two different matching strategies exist side by side on purpose —
  `DeduplicateBaseName` (exact stem match, checked once before downloading, against
  *other* files) and `CleanupArtifacts` (prefix match, checked after a failure, against
  *this job's own* artifacts). Documented here so the asymmetry doesn't look like an
  oversight.

### Native folder picker deferred to a later phase
- **Context:** spec section 36 says "if straightforward, implement a native directory
  picker," and section 11 explicitly allows "a simple working path selection mechanism"
  for Phase 2.
- **Options considered:** (a) add `tauri-plugin-dialog` (native folder picker); (b) a
  plain text input for the output directory.
- **Choice:** (b).
- **Reason:** Phase 1 already hit a real MinGW/Tauri linker limitation from the
  dependency graph being too large (see the MinGW decision above) — adding another Tauri
  plugin increases that graph further for a feature the spec itself says can wait. Not
  "straightforward" enough given that history to justify the risk in a vertical-slice
  phase.
- **Consequences:** `DownloaderPage` uses a plain text field for the output directory
  (spec-compliant for Phase 2). Revisit when the UI work in `docs/roadmap.md` "Phase 8"
  begins, or sooner if a lighter-weight picker approach is found.

### "Open containing folder" via a plain Rust `explorer.exe` invocation, no new plugin
- **Context:** spec section 37 wants a completed download to offer "open containing
  folder," implemented through the backend rather than an arbitrary shell command from
  React.
- **Choice:** a small Tauri command in `app/desktop/src-tauri` that validates the path
  exists, then runs `Command::new("explorer").arg(format!("/select,{path}"))` — one
  argv entry, no shell interpretation, no new crate dependency.
- **Reason:** achieves the spec's requirement (backend-mediated, not a raw shell string
  built from user input) without adding to the dependency-graph risk noted above.

### Python error classification stays a heuristic string-match table
- **Context:** yt-dlp doesn't expose a stable, machine-readable error-code enum for
  failure kinds (private video, removed, geo-restricted, etc.) — only exception message
  text.
- **Choice:** `classify_exception()` in `downloader.py` matches known substrings
  (`docs/protocols/downloader.md` has the full table) with a fallback to
  `E_DOWNLOAD_FAILED`/`UNKNOWN` for anything unmatched.
- **Consequences:** this is explicitly acknowledged as a heuristic, not a guarantee — a
  yt-dlp wording change could cause a failure to fall through to the generic bucket
  instead of a specific one. That's an acceptable degradation (the user still gets a
  clean, structured error) rather than a crash or a misleading message.

### Logger's console sink moved from stdout to stderr (bug fix, not a forward-looking decision)
- **Context:** found during Phase 2's first real end-to-end IPC test — the very first
  byte written to `mediatool-core.exe`'s stdout was a plain-text `spdlog` line
  ("IPC loop starting"), not NDJSON, because `Logger::Init()` (Phase 1 code, unchanged
  since) built its console sink as `spdlog::sinks::stdout_color_sink_mt`. Every
  `Log::Info/Warning/Error` call during `RunIpcLoop()` was silently corrupting the
  protocol stream this way — undetected in Phase 1 because `--selftest` never calls
  `RunIpcLoop()` and the GoogleTest suite never runs the IPC loop as a subprocess either.
- **Fix:** changed the console sink to `spdlog::sinks::stderr_color_sink_mt`. stdout is
  now, and was always meant to be, exclusively the NDJSON channel — the same convention
  `downloader.py` already followed for the same reason.
- **Why recorded here:** this is exactly the kind of latent bug that only surfaces under
  real integration testing, not unit tests in isolation — recorded so a future phase
  doesn't accidentally revert it while "simplifying" logging setup.

### Python test suite stays `unittest`, not `pytest`
- **Context:** spec section 40 suggests "use pytest where appropriate."
- **Choice:** kept Phase 1's dependency-free `unittest` convention for
  `tests/python/test_downloader_protocol.py` rather than adding `pytest`.
- **Reason:** Phase 1 deliberately runs these tests under an ambient interpreter that
  does NOT have `yt_dlp` (or now, deliberately, `pytest`) installed, specifically so the
  suite can verify the "yt_dlp is not installed" code path for real rather than mocking
  it. Adding pytest as a dependency to that ambient environment would work fine
  (`pip install` is within the user's stated toolchain constraints) but breaks
  consistency with the existing suite for no functional benefit.
- **Consequences:** documented here per spec section 47 as a deliberate deviation from
  the letter of section 40, in keeping with its spirit (thorough, isolated, no-network
  protocol testing).

## Phase 4

### "Zero-Copy Transfer" (4.9) — not applicable to this architecture, dropped
- **Context:** the must-have feature list asked for a "Zero-Copy Transfer" capability
  alongside Watch Folders, Scheduled Tasks, Hotkeys, Notifications, Presets, Hardware
  Acceleration, and Parallel Processing.
- **Options considered:** none — there was no ambiguity to resolve once the actual data
  flow was traced. `ffmpeg`/`ffprobe` and yt-dlp read and write media bytes directly on
  disk as independent OS processes; Gravity itself never holds a media buffer in memory to
  begin with. Everything that crosses a process or IPC boundary in this app (React <->
  Rust <-> C++ core <-> engines) is small JSON control/progress messages — job parameters,
  `-progress pipe:1` lines, NDJSON events — never raw audio/video/image bytes.
- **Choice:** treat this as not applicable rather than build a placeholder or an inert
  stub for it. There is no raw-media copy anywhere in the pipeline to
  eliminate, so there is nothing a "zero-copy" mode could actually turn off.
- **Consequences:** no code, no UI affordance, no IPC surface for this item — this
  paragraph is the entire close-out. If a future architecture change ever did route raw
  media bytes through a Gravity-owned buffer (e.g. an in-process filter chain instead of
  shelling out), this decision should be revisited then, not before.

## Phase 5

### FFmpeg vendor: BtbN/FFmpeg-Builds, not gyan.dev — correcting an earlier planning assumption
- **Context:** the original plan (locked decision #1) specified vendoring gyan.dev's
  "essentials-shared" build as "LGPL-labeled." Verifying this while writing
  `docs/licensing.md` found it was wrong: gyan.dev's "essentials", "full", and
  "full-shared" builds are all **GPLv3** (they bundle `libx264`), and gyan.dev does not
  publish a ready-made LGPL artifact at all.
- **Options considered:** (a) proceed with gyan.dev's shared build anyway, accepting it's
  GPL, and relax decision #1's "no GPL codecs bundled" claim; (b) find and vendor a build
  from a source that actually ships a genuine LGPL variant.
- **Choice:** (b) — [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds), which
  publishes explicit `*-lgpl-shared`/`*-lgpl` variants alongside its GPL ones specifically
  to serve this use case. See `docs/licensing.md` for the exact artifact.
- **Reason:** (a) would have silently broken the entire licensing strategy this project
  was built around — shipping a GPLv3 FFmpeg while documenting "LGPL, no GPL codecs
  bundled" is a real compliance defect, not a rounding error. Catching it in the docs pass
  before Phase 5.2 actually vendors a binary means the fix costs nothing; catching it after
  shipping would not.
- **Consequences:** `docs/licensing.md`'s pinning note documents why no SHA256 is recorded
  yet (BtbN publishes under a rolling `latest` release tag with no stable pre-hash URL) —
  the real pin happens when `scripts/vendor_ffmpeg.ps1` (Phase 5.2) actually downloads and
  hashes the artifact on a real build.

### `bundle.resources` lives in a separate `tauri.release.conf.json`, not the base config
- **Context:** wiring up Phase 5.2's sidecar bundling, adding `bundle.resources` entries
  for `mediatool-core.exe`/ffmpeg/Python directly into `tauri.conf.json` broke `cargo
  check` entirely: Tauri's build script validates every configured resource path actually
  exists on disk, unconditionally, on every compile -- not just when actually running
  `tauri build`. Confirmed by reproducing it locally: `cargo check` failed with `resource
  path 'resources/ffmpeg' doesn't exist` the moment the config was added, with nothing in
  `resources/` yet (which is correct -- it's populated by the Phase 5.2 vendor scripts,
  not committed).
- **Options considered:** (a) commit placeholder files (empty `.gitkeep`s, a stub
  `mediatool-core.exe`) so the paths always exist; (b) split the resources config into a
  separate file merged in only for the actual packaging command, via Tauri CLI's
  `--config` (JSON Merge Patch over the base config).
- **Choice:** (b) -- new `app/desktop/src-tauri/tauri.release.conf.json`, containing only
  the `bundle.resources` object, passed as `npm run tauri build -- --config
  tauri.release.conf.json`.
- **Reason:** (a) would have meant nobody could `npm run tauri dev` or even `cargo check`
  on a fresh clone without first vendoring 200+ MB of binaries -- a placeholder `.exe`
  committed to git to work around that is also just an ugly, confusing thing to find in a
  source tree. (b) keeps the base config (and therefore the entire existing dev loop)
  completely unaffected, since dev commands never pass `--config` and never see the
  resources key at all -- verified locally both ways (`cargo check` clean with the base
  config; `TAURI_CONFIG=<release config json> cargo check` clean once placeholder resource
  files existed).
- **Consequences:** `docs/development.md`'s "Packaging" section and
  `scripts/prepare_bundle_resources.ps1`'s final instructions both reference the
  `--config` flag explicitly -- anyone packaging the app for real must remember it, or the
  build silently ships without the bundled resources at all (still an `active: true`
  NSIS bundle, just missing `externalBin`/resources, functionally identical to the #7
  audit finding this phase exists to fix). Worth a CI/build-script check later that the
  final `.exe`'s resource dir actually contains what's expected, not just that `tauri
  build` exited zero.

### Gravity's own source LICENSE — deliberately left unresolved
- **Context:** the plan flagged this as the user's call, not a default to pick.
- **Choice:** asked directly (proprietary/all-rights-reserved vs. source-available vs.
  permissive open source vs. skip) — the user chose to skip it for this pass.
- **Consequences:** no root `LICENSE` file exists. The repository's default legal state
  ("all rights reserved") applies by absence of a file, not by a considered choice — `docs/licensing.md` flags this explicitly so it isn't
  mistaken for a deliberate "we chose all-rights-reserved" decision later. Revisit before
  any public source distribution.

## Hardening pass (concurrency & IPC architecture)

Context for the whole section: the audit's concurrency defects (#3, #4, #5, #6, #12) had
each been fixed individually. This pass went after the shapes that let them exist at once
— check-then-act on job state, one thread doing both I/O and request routing, scheduling
policy tangled up in the thing that owns the threads. See `docs/concurrency-model.md`.

### Job transitions return a result instead of throwing
- **Context:** `MarkStarting()` threw `E_INVALID_JOB_TRANSITION` when it lost a race with a
  concurrent cancel. The loser of that race is a worker thread, where an escaping exception
  is `std::terminate` (audit #4). The first fix caught the exception in `RunJob` and
  matched on its error *code* to decide whether it was a benign race.
- **Options considered:** (a) keep throwing, keep catching; (b) return a `TransitionResult`
  enum distinguishing Success / AlreadyInState / AlreadyTerminal / InvalidTransition.
- **Choice:** (b). Every transition entry point on `Job` returns one and none of them throw.
- **Reason:** "the transition didn't happen" is a normal outcome under concurrency, and
  encoding a normal outcome as an exception across a thread boundary is what made a routine
  race fatal. Matching on an error-code string to recover the distinction was a symptom of
  the wrong return type. The enum also removes the check-then-act shape from callers:
  `PauseJob`/`ResumeJob`/`RetryJob` now attempt the transition and report its result
  instead of reading `State()` and acting on what it said a moment ago.
- **Consequences:** callers must inspect the returned value; a `Mark*` call whose result is
  ignored is a silent no-op if it loses a race. `JobManager` logs `InvalidTransition`
  (a real bug) and quietly declines on `AlreadyTerminal` (a lost race).

### Scheduling policy extracted into a threadless `SchedulerCore`
- **Context:** issue #17 asked for priorities, dependencies and a retry policy. `JobManager`
  *was* the scheduler: a `std::deque` its workers popped, with priority as an insertion
  tweak. Every scheduling question could only be answered by starting threads.
- **Options considered:** (a) add dependency tracking to `JobManager` alongside the threads;
  (b) extract a pure class that decides what runs next and knows nothing about threads,
  locks or `Job`.
- **Choice:** (b). `JobManager` holds a `SchedulerCore` under its existing mutex.
- **Reason:** the policy is where the interesting logic is and the threads are where the
  risk is; keeping them in one class meant neither could be tested properly. The scheduler's
  ~30 tests start no threads at all.
- **Consequences:** `SchedulerCore` is not thread-safe by itself, on purpose — there is one
  lock in this subsystem, not two that could be taken in two orders. Dependencies must name
  an already-submitted job, which is also what makes cycles impossible rather than merely
  detected. A job whose dependency fails is cancelled, not failed: `QUEUED -> FAILED` isn't
  a legal transition and inventing one to carry a "your dependency failed" error would be a
  bigger change to the state machine than the message is worth.

### Blocking IPC commands run on a bounded executor, not the request loop
- **Context:** `inspectDownloadUrl` blocks on a yt-dlp subprocess. Issue #8's fix bounded
  that wait at 30 seconds, which bounds how long the *whole backend* stops reading stdin —
  it doesn't stop it happening.
- **Options considered:** (a) make inspect asynchronous at the protocol level (respond
  `{status: "pending"}`, deliver the result as an event); (b) a timeout on the Rust side;
  (c) keep the protocol identical and run the blocking handlers on a small pool.
- **Choice:** (c).
- **Reason:** responses are already correlated by `id` and the contract already says
  requests may complete out of order, so (c) needs no change to the wire protocol, the Rust
  bridge or the frontend — the loop simply stops waiting. (a) is the same behavior with a
  new protocol surface and two more states for the frontend to model; (b) hides a hung core
  rather than keeping it responsive.
- **Consequences:** handlers that can run on an executor thread must be thread-safe, which
  is why the settings and preset stores are now serialized. A saturated executor answers
  `E_CORE_BUSY` rather than queueing without limit or blocking the loop.

### The frontend orders snapshots by event sequence, not by arrival
- **Context:** issue #19 removed the per-progress-event `getJob` round trip, which fixed the
  visible progress rewind. The remaining lifecycle events still each fire their own async
  `getJob`, and nothing ordered those responses.
- **Choice:** stamp every handled event with a monotonically increasing sequence number,
  carry it through whatever it causes to be applied, and drop an update if something newer
  has already been applied to that job.
- **Reason:** core events arrive over one stdout stream in one order; the client only ever
  had to stop undoing it. A queue or a mutex in the UI layer would serialize round trips
  that are fine to run concurrently — the problem was never concurrency, it was ordering.
- **Consequences:** an update that loses the comparison is discarded, not retried. That is
  correct (something strictly newer is already displayed) but it does mean a snapshot fetch
  can be wasted work; the alternative is showing the user a state their app has already
  moved past.

## Defect investigation pass (issues #79-#86)

### Compression is a bitrate target derived from the source, not a CRF quality target
- **Context:** issue #80 -- a 1.17 MB file compressed to 3.46 MB, identically at *both*
  high and low quality. Two independent causes, both reproduced with a real ffmpeg here:
  1. `-crf` is a *quality* target, not a *size* target. "Compress" was byte-for-byte the
     same ffmpeg invocation as "Convert", with no reference to the input's size at all.
     Measured on a 1.19 MB 640x360 H.264 clip: CRF 23 ("medium") returned 1.03x the
     original, CRF 18 ("high") returned 1.41x. Re-encoding already-compressed material at
     a fixed CRF inflates it routinely, not exceptionally.
  2. `libopenh264` -- the bundled default H.264 encoder, and therefore the one almost
     every install uses -- **defines no `crf` AVOption**. ffmpeg does not fail on the flag:
     it logs "Codec AVOption crf ... has not been used for any stream" at `AV_LOG_WARNING`,
     which this codebase's own `-loglevel error` suppresses, and exits 0. The encode then
     runs at `TARGET_BITRATE_DEFAULT` (2 Mbps, hardcoded in ffmpeg's `libopenh264enc.c`)
     for every quality tier alike. 2 Mbps over the reported clip length is ~3.46 MB, and
     it is identical at high and low quality -- exactly the reported symptom.
- **Options considered:** (a) lower the CRF values for compression -- rejected, it cannot
  *promise* anything about size and does nothing at all on libopenh264; (b) switch the
  bundled encoder -- rejected, the libopenh264 choice is licensing-load-bearing
  (`docs/licensing.md`) and unrelated to this bug; (c) derive an explicit bitrate from the
  source.
- **Choice:** (c). `MediaProcessingJob` probes the input before encoding and passes
  `videoBitrateKbps` (or, for an audio-only target, `audioBitrateKbps`) computed as
  source bitrate x a per-tier factor -- 0.20x at "lowest" up to 0.75x at "ultra" for a
  compression job. `BuildFfmpegArgs` emits `-b:v/-maxrate/-bufsize` when a target is
  present, and emits `-crf` only for an encoder that actually implements it
  (`EncoderSupportsCrf`), never both.
- **Consequences:** compression is smaller than its input by construction rather than by
  luck. Measured end-to-end against the same 1.19 MB source: 0.24x / 0.29x / 0.44x / 0.59x
  / 0.75x across the five tiers. A source ffprobe reports no bitrate for gets no target at
  all -- prior behavior, rather than a number invented from one we don't have. The quality
  tier now also has an effect on the libopenh264 path, which it never did before.

### The downloader's Python paths are anchored to the executable, not the working directory
- **Context:** issue #79 -- `E_PROCESS_LAUNCH_FAILED` / "The system cannot find the file
  specified" naming a `python.exe` that exists. Both the interpreter and `downloader.py`
  were resolved as an env var *or* a CWD-relative literal
  (`python/downloader/.venv/Scripts/python.exe`), handed straight to `CreateProcess`, and
  never checked. The core's working directory is whatever the Tauri shell inherited --
  `app/desktop/src-tauri` under `tauri dev`, the shortcut's "Start in" for an installed
  build -- essentially never the repository root that literal was written against.
- **Choice:** `core/filesystem/ToolPathResolver` builds an ordered candidate list (explicit
  env override, then each relative candidate against the core executable's own directory
  and its ancestors, then the legacy CWD-relative form last so nothing that resolved before
  stops resolving), existence-checks each one, and cleans surrounding quotes off env values
  (`set VAR="C:\..."` in a batch file stores them, and they then reach `CreateProcess` as
  part of the filename while `echo` still looks correct).
- **Consequences:** a missing interpreter no longer aborts startup -- downloading is one
  feature among several -- but the next download or inspect fails with
  `E_DOWNLOADER_NOT_FOUND` listing every path that was tried. **Not verified on Windows
  from this environment**; the CWD-anchoring defect is verified by unit test, and the other
  candidate causes are converted from silent failures into named ones rather than proven
  absent.

### Context-menu entries reference Tauri's `${MAINBINARYNAME}`, never a literal
- **Context:** issues #85 and #52 -- right-click "Convert"/"Compress" showed Windows'
  generic "How do you want to open this file?" chooser. Two prior passes reviewed
  `hooks.nsh` and `cli.rs` and found nothing, concluding a Windows machine was required.
- **Root cause:** `hooks.nsh` registered `"$INSTDIR\Gravity.exe" --convert "%1"`, but Tauri
  names the installed binary after `mainBinaryName`, which defaults to the **Cargo package
  name** (`gravity-desktop`), not `productName` (`Gravity`) -- `mainBinaryName` is unset
  here, and `cargo metadata` confirms the produced bin target is `gravity-desktop`. Every
  verb pointed at a file the installer never creates, so Explorer fell back to the chooser.
  Tauri's own template still carries the comment "We used to use product name as
  MAINBINARYNAME" over its shortcut-migration code, which is where the stale convention
  came from.
- **Choice:** reference `$INSTDIR\${MAINBINARYNAME}.exe`, the define Tauri itself uses, so
  the two can never drift. Tauri `!include`s the hook file *before* it `!define`s
  `MAINBINARYNAME`, which is fine: NSIS expands `${...}` in a macro body at
  `!insertmacro` time. Verified by compiling this exact file with `makensis` against a
  harness reproducing that ordering -- the emitted string is
  `\gravity-desktop.exe" --convert "%1"`, with no warnings.
- **Consequences:** a Rust unit test now fails if any `$INSTDIR` line in `hooks.nsh`
  hardcodes a binary filename again. Still unverified against a real installed build on
  Windows.

### "Open folder" passes explorer.exe a raw argument
- **Context:** issue #84 -- "Open folder" could not locate a completed job's output. Issue
  #38 had wrapped the path in quotes (explorer parses `/select,<path>` by its own rules, so
  a comma in a filename selects the wrong item). But Rust's Windows argument encoder
  escapes every `"` in a regular argument *unconditionally* -- `append_arg` in std's
  `sys::args::windows` inserts a backslash before each one whether or not the argument gets
  quoted -- so `Command::arg` turned `/select,"C:\out\clip.mp4"` into a command line
  reading `/select,\"C:\out\clip.mp4\"`. Explorer received a path with literal
  backslash-quotes and could not find it. The #38 fix defeated itself.
- **Choice:** `CommandExt::raw_arg`, which appends the fragment verbatim with no quoting or
  escaping -- the documented escape hatch for exactly this. Separators are also normalized
  to backslashes, since an output directory typed as `D:/Converted` reaches the command as
  typed and explorer will not select through a forward slash.

### The "Pro" tier is removed rather than left inert
- **Context:** issue #82. `idealist.md` had called for Pro affordances built
  "visibly-present-but-inert", and the code followed: a `ProLockedBadge` component, four
  disabled stub controls on the Convert page, and a server-side `E_PRO_FEATURE_LOCKED`
  rejection of `quality: "lossless"`.
- **Choice:** delete all of it. The badge component and the batch/trim/watermark stubs are
  gone (they were wired to nothing), and `lossless` is now an ordinary selectable quality.
  The vision text in `idealist.md` is marked superseded rather than deleted -- the feature
  ideas still stand, only the tiering framing is gone.
- **Consequences:** with the badge removed, an inert control would read as a broken one, so
  the stubs had to go with it rather than being left unlabelled. Trim and watermark remain
  implemented in `BuildFfmpegArgs` with no UI to reach them; that is a roadmap gap now
  rather than a fake paywall.


## Hardening & feature-completion pass (phases A-F)

### A deferred operation is declared, not merely true

`getCapabilities` advertised `extractAudio` and `extractFrames` for every video while both
`FFmpegEngine` and `MockMediaEngine` threw `E_NOT_IMPLEMENTED` from them. Nothing was
wrong with either half on its own; what was wrong is that they were two independent
statements about the same fact, and only one of them reached the frontend. The only way to
discover an operation did not run was to start a job and read the failure.

`core/media/DeferredOperations.h` is now the single table, and both halves are derived from
it: `filesystem::DeferredCapabilitiesFor()` reports the operation with a user-facing reason
in a list disjoint from `CapabilitiesFor()`, and every engine throws
`MakeNotImplementedError()`, whose `message` *is* that same reason. The deferral cannot rot
into a lie because implementing an operation means deleting its table entry, which fails
the tests that assert the deferral.

The alternative — implementing `ExtractAudio` for real — was rejected as out of scope for
this pass and, more importantly, as not the actual defect: `convert` to an audio format
already does what a user means by "extract the audio", so the missing feature was costing
far less than the dishonest capability list.

### A metadata fetch gets a wall-clock deadline; a download does not

`downloader.py` already sets yt-dlp's `socket_timeout`, and that is not a bound on the
caller: it limits one connect/read, a fetch is many of them, and a child that keeps
trickling data never trips it. `Inspect()` runs synchronously on a job worker, so a wedged
child held that thread until the process exited.

`YtDlpProvider` now enforces a `steady_clock` deadline (60s, injectable) around the whole
run, stops the child, and reports `E_INSPECT_TIMEOUT` as recoverable. `Download()`
deliberately gets no such deadline: a legitimate 4K download runs for as long as it runs,
and a clock that kills it is a bug, not a safeguard. Bounding a download is about
*silence*, not duration, which is a different mechanism (see the long-running-job timeout).

### `formatId` is validated as a name because `-f` is a language

`downloads::DownloadOptions::formatId` comes from the frontend and reached yt-dlp's `-f`
verbatim. `-f` accepts filter expressions, fallback chains, arithmetic and the `all`
keyword — so an unvalidated value there is not a free-text field, it is code: `all`
downloads every stream on the page and a filter selects something the user never saw.

It is now held to the shape of an actual format id (up to eight `+`-joined ids of
`[A-Za-z0-9_.-]`), which is exactly what `Inspect()` reports and what a caller can build
from two of them. The preset-derived selectors are ours and are explicitly *not* subject to
this — they legitimately use the expression syntax. `all` and `mergeall` are rejected by
name; the other bare keywords are not, because each still resolves to one stream of the
same video and a site may legitimately name a format id after one of them.

### Crash recovery restores intent, not progress

`JobHistoryStore`'s header states that persisting in-flight jobs is a non-goal, and the
reasoning it gives — that resuming an ffmpeg or yt-dlp process across a restart is high
complexity for little value — is still correct. It is not, however, the same question as
"should the queue survive a crash".

`core/jobs/InProgressJobStore` persists a `JobSpec` — the `createJob` request that produced
each unfinished job — and drops it the moment the job reaches a terminal state. On startup
those specs are replayed through the same `SubmitJobOfType()` the live path uses, so a
recovered job is constructed and validated identically to a fresh one instead of through a
second path that can drift.

The guarantee is deliberately the weak one, stated plainly: **a killed job is rebuilt and
re-run from the start, not resumed.** Its subprocess died with the process, its partial
output is not a checkpoint, and neither yt-dlp nor ffmpeg offers a resume protocol this app
could drive. What a user actually loses today is the queue — twenty things lined up and
nothing to say what they were — and that is what comes back.

Three consequences worth recording:

* **Ids change.** A rebuilt job is a new `Job` object with a new id, so `dependsOn` edges
  are remapped during the replay. An id that is not in the remap belonged to a job that
  already reached a terminal state, so the edge is either satisfied or unsatisfiable —
  dropping it is what lets the dependent run at all.
* **Recovery is capped.** A job that takes the process down on every attempt would
  otherwise re-queue itself on every launch. `kMaxRecoveryAttempts` (3) stops it.
* **The store is cleared before the replay**, and each job re-adds itself through the
  normal submission path. A spec that can no longer be built — its input file is gone, say
  — is then dropped with a logged reason rather than retried forever. The gap this leaves
  is a crash *during* recovery, which is not the failure the file exists to survive.

### Orphan cleanup targets output artifacts, not the unused temp directory

The obvious reading of "orphaned temp-file cleanup" is a sweep of
`%LOCALAPPDATA%\MediaTool\temp\job-*`. `core/filesystem/TempDirectory` is RAII and would
indeed leak those across a kill — except that nothing in the codebase constructs one. A
sweep there would clean up something that never exists.

What a killed run actually leaves behind is in the user's *output* directory: yt-dlp
`.part` files and half-written encodes. Their names are derived on the worker thread (from
a video title, or an input stem, plus collision-avoidance suffixes), so nothing outside the
run knows them. Both job types now report the reserved base name through an
`onArtifactLocation` hook the instant they reserve it, the recovery store records it, and
the recovery pass deletes those artifacts — through the same `IsJobArtifactOf` scoping every
live failure path already uses, never a bare prefix match, never recursive — before
resubmitting.

### `JobManager` is declared last in `AppContext`, and that is load-bearing

`~JobManager` cancels every queued job and joins the worker pool, which fires state-changed
callbacks — and those callbacks touch `jobHistoryStore`, `inProgressJobStore`,
`previousState` and `previousStateMutex`. Members are destroyed in reverse declaration
order, so `JobManager` sat above the stores meant a shutdown with jobs still queued wrote
history into a destroyed object. Moving it to the end of the struct is the whole fix, and
the comment there says why nothing may be declared after it.

### Artifact cleanup handlers catch `...`, not `MediaToolException`

Both job types cleaned up after a failed engine/provider call under
`catch (const errors::MediaToolException&)`. Anything else escaping — `std::bad_alloc`, a
`nlohmann::json` exception, a `std::runtime_error` from a future provider — skipped cleanup
entirely, which is precisely the case where the half-written file outlives every code path
that still knows its name. The handler rethrows unchanged; only the cleanup is
unconditional.

### Automatic retry needs two signals, not one

The obvious design keys retry off `ErrorInfo::recoverable` alone, which the plan called
for. That gets one class of failures wrong in each direction, so the implemented rule
requires both signals to agree.

`recoverable` is set by the layer that produced the failure, and it is the only thing that
can distinguish a 503 from a DNS name that will never resolve — a category cannot. But it
is also the flag most likely to be set carelessly, and a provider that marks a full disk
"recoverable" turns one clear failure into three identical ones several seconds apart. So
category acts as a veto over deterministic failures — `FILE_NOT_FOUND`, `INVALID_FILE`,
`UNSUPPORTED_FORMAT`, `PERMISSION_ERROR`, `DISK_SPACE_ERROR`, `CANCELLED` — regardless of
the flag, and `recoverable` is the positive signal within the categories that remain.

`CANCELLED` is in that veto list for a different reason than the others: retrying it is not
wasted work, it is the app arguing with the user.

### An automatic retry never passes through FAILED

`JobState` already had `RETRYING`, reachable only from `FAILED` — the manual path, a user
pressing Retry on a job that gave up. Reusing it for automatic retry would have meant a
retried job briefly entering a terminal state, and `FAILED` is terminal in ways that are
not cosmetic: `SchedulerCore::RecordTerminal` cancels every job that `dependsOn` it, and
`JobHistoryStore` records the failure. An entire dependent chain torn down, and a permanent
history entry written, for an attempt the next scheduling decision was about to repeat.

So `RUNNING -> RETRYING` was added as its own transition. The job never becomes terminal, no
dependent is stranded, no history is written, and the frontend gets a `jobRetrying` event
carrying the attempt number, the limit and the error rather than watching a job flicker
through `FAILED`. Only a job that exhausts its attempts — or fails in a way the policy will
not retry — reaches `FAILED`, which is now the only terminal failure there is.

`RETRYING -> CANCELLED` was added at the same time, and `Job::RequestCancel` now finalizes a
`RETRYING` job on the spot the way it already did a `QUEUED` one. Both states have no worker
thread inside `Execute()` to notice the cancellation flag, so nothing else would ever
finalize them — and a backoff is exactly when a user gives up on a job, so having to wait
out a 30-second timer before Cancel took effect would make it feel broken.

### The backoff lives in the scheduler, not in a timer thread

Waiting out a backoff needs something to wake up when it elapses. The obvious options were a
dedicated timer thread or a sleeping worker, both of which add a second concurrency
mechanism to a subsystem whose whole design is "one lock, one class that touches threads".

Instead `SchedulerCore` gained a `notBefore` per pending entry and takes the current time as
an *argument*: `TakeNextEligible(now)`, `HasEligible(now)`, `NextEligibleTime(now)`. It stays
threadless and clockless, so the entire backoff schedule is testable as a sequence of
function calls with no real waiting — the same property that made priorities and
dependencies testable. `JobManager`'s workers then sleep with `wait_until(NextEligibleTime)`
rather than `wait`, recomputing the deadline on every pass so a shorter backoff scheduled
while a worker slept shortens the sleep.

`TimePoint::max()` is the default `now`, meaning "time is not a constraint", so every
existing call site and test that never schedules a backoff is unaffected and does not have
to think about it.

### `maxRetryAttempts` is read with `value()`, not `at()`

Every other field in `Settings::FromJson` uses `at()`. That is fine for a field that has
always existed and wrong for one being added: `LoadFrom` catches a parse failure by falling
back to `Settings::Defaults()`, so a settings file written before this field existed — a
completely normal thing to find on disk after an upgrade — would have silently discarded
every setting the user had chosen. An additive field must default, not fail.

### Integration and stress suites are separate binaries, not more files in `mediatool_tests`

The unit suite is fast (about 15 seconds) and that is why it gets run constantly. Adding a
suite that spawns real subprocesses, and another that submits hundreds of jobs across four
worker threads, would have taken that away from every developer on every build to buy
coverage that is only meaningful when run deliberately.

So `mediatool_integration_tests` and `mediatool_stress_tests` are their own executables,
gated behind `scripts/ci-local.ps1 -IntegrationTest` / `-StressTest`. CMake passes the
built `mediatool-core` path into the integration binary as a compile definition
(`$<TARGET_FILE:mediatool-core>`), so it always drives the binary that was just built
rather than whatever happens to be on PATH.

The integration tests redirect `LOCALAPPDATA` into a per-test temporary directory, so the
child's settings file, job history and in-progress-job store are never the developer's
real ones, and point `MEDIATOOL_PYTHON_PATH`/`MEDIATOOL_DOWNLOADER_SCRIPT` at real existing
files so the downloader-availability gate passes and the validation behind it is reachable.

What they cannot cover on a POSIX build host is recorded in the file itself: anything
needing a path to survive `IsSafeUserSuppliedPath`, which requires a Windows-shaped
absolute path by design. Those paths work on Windows and here can only be exercised in
their rejecting direction.

### The IPC integration tests found a real defect on their first run: `RealProcessRunner` writes

`RealProcess`'s header states the invariant plainly -- reproc does not support two threads
operating on one process handle, so **only** the drain thread may touch `process_`, which is
why `Terminate()`/`Kill()` set a flag for that thread instead of acting directly.
`WriteLine()` and `CloseStdin()` did not follow it: both called into `process_` from the
caller's thread.

The consequence was invisible in production and immediate under test. The only production
caller, `YtDlpProvider::RunPythonCommand`, writes exactly one command line and then closes
stdin, so a *second* write to a live child had never happened. The IPC integration tests
write repeatedly, and the second write to a running `mediatool-core` fails with
`REPROC_EINVAL` — reproducibly, from the first run of the first test.

Writes and the stdin close are now queued and performed by the drain thread, with the
caller blocking until the result is known so `WriteLine` keeps its synchronous
"wrote it, or threw" contract. Two things fell out of the fix:

* **Partial writes were being discarded.** `reproc::process::write()` reports how much it
  managed to write, and the old code ignored that count, silently truncating anything the
  pipe could not take in one go. The queued path loops until the buffer is written, which is
  what makes the 4 MB oversized-line test meaningful rather than accidentally passing.
* **The poll interval dropped from 200ms to 50ms.** A queued write waits at most one poll
  interval, and the same interval already bounded how quickly `Terminate()`/`Kill()` take
  effect — so the shorter window helps both. The integration suite went from 29s to 4.8s.

### Stress tests assert outcomes, never durations

There is exactly one timing assertion in the stress suite, and it is a 10-second ceiling on
shutting down a manager with 500 queued jobs -- a bound that separates "cancelled the queue"
from "ran the queue", not a performance target. Everything else asserts that every job
reached a terminal state, that no job ran more times than the retry policy allows, and that
no snapshot was ever torn.

A stress test that fails on a loaded CI box teaches people to re-run it until it passes,
which is worse than not having it.

### ffmpeg's stderr was the missing information all along

`RunFfmpegJob` passed a callback literally named `ignoreStderr`, so every ffmpeg failure
became `E_FFMPEG_FAILED` plus an exit code. "The disk is full", "that folder is read-only",
"this file is corrupt" and "your ffmpeg build has no such encoder" were the same error to
the frontend, to the user, and to the retry policy — and the text distinguishing them was
on the child's stderr the entire time, being thrown away.

A bounded tail is now kept and run through `engines/ffmpeg/FFmpegErrorClassifier`, a
substring table over ffmpeg's own prose. It is a heuristic, for the same reason
`downloader.py`'s is: ffmpeg has no stable machine-readable error vocabulary, and exit codes
carry even less. Strings were verified against ffmpeg 6.1.1's real output where they could
be produced locally (a truncated mp4, an unknown encoder, a missing input); the
errno-derived ones come from ffmpeg printing `strerror()` through `av_err2str`, the same
path that produced the confirmed "No such file or directory". Anything unrecognized keeps
the generic code and the stderr tail rather than being guessed at.

Each classification decides three things the call site cannot: the code, the **category**
— which is what `RetryPolicy` vetoes on, so getting `InvalidFile` rather than `EngineFailure`
is what stops a corrupt file being re-encoded three times — and what the user should try
instead. A failed *probe* uses the same table with a different default: ffprobe being unable
to read a file is the definition of "not a media file", so an unrecognized probe failure is
`E_INVALID_FILE`, not a generic engine failure.

### The watchdog cancels; it does not force-kill, and says so

The plan called for force-killing jobs past 12 hours after a 30-second grace period. Half of
that is implementable and half is not, and it is worth being precise about which.

Requesting cancellation is genuinely effective: `DownloadJob` and `MediaProcessingJob` poll
`IsCancellationRequested()`, and both engines terminate — then kill — their child process on
seeing it, so a wedged ffmpeg or yt-dlp really does die and the slot really is freed. What
cannot be done is force a job whose `Execute()` ignores cancellation to return. That thread
belongs to the job, and there is no portable way to reclaim it without killing the process.

So after the grace period the watchdog logs the job as unresponsive and leaves it alone. The
alternative — marking it `FAILED` while its worker thread is still running — would report a
worker slot as free when it is not, which is a worse failure than the one being reported.

The watchdog sleeps on the same condition variable the workers use, so `Shutdown()` wakes it
immediately rather than waiting out its 30-second interval, and it measures the **current
attempt** (`Job::RunningFor()`, on `steady_clock`) rather than the job's lifetime — a job on
its third retry has not been running for the sum of its attempts, and treating it that way
would cancel healthy retries.

### The downloader health probe is lazy, and never fatal

yt-dlp's extractors for the big sites break on a scale of weeks. A build a couple of years
old does not fail cleanly — it fails on real URLs with "video unavailable", blaming the
video for what is actually a stale tool. Since yt-dlp's version string is a release date,
its own staleness is computable.

The probe runs at most once per process and only when something asks, rather than at
startup: it starts a process, which would slow every cold start for information most
sessions never need, and it would run before the user has had a chance to point
`advanced.ytDlpPath` somewhere that works. It is never fatal, in either direction — a
missing yt_dlp comes back as `available: false` rather than an exception (that is the
question being asked), and a stale one warns rather than refusing a download the user might
well get.

### The watchdog needs its own condition variable, not the workers'

Adding the watchdog to `queueCv_` — the same variable the worker pool waits on — made it a
competing waiter, and `SubmitJob`/`RetryJob` wake a worker with `notify_one`, which picks an
arbitrary waiter. So a submission could wake the watchdog instead: it checked `stopping_`,
found it false, went back to sleep, and had consumed the wakeup. The queued job then sat
there until something else happened to notify.

It failed loudly and immediately -- three retry tests and two IPC integration tests went
from passing to timing out -- which is the argument for having written them. `watchdogCv_`
uses the same `mutex_` (it reads `jobs_`) but is signalled only by `Shutdown()`, so the
two wakeup paths cannot steal from each other.
