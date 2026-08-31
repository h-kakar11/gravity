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

### Playlist URLs: rejected with `E_PLAYLIST_NOT_SUPPORTED`, not implemented (issue #41)
- **Context:** `downloader.py`'s single-video probe options (`noplaylist`, `extract_flat:
  "in_playlist"`) and `WithPlaylistIndex`/playlist-count scaffolding already exist in the
  data structures, but nothing above them ever decomposes a playlist into per-video jobs --
  a bare playlist URL is rejected outright. `docs/roadmap.md` referenced this decision
  without it actually being recorded anywhere, which is what issue #41 flagged.
- **Options considered:** (a) implement full playlist support now -- decompose a playlist
  URL into one queued DownloadJob per entry, using the existing `WithPlaylistIndex`
  numbering; (b) keep rejecting playlist URLs and say so explicitly, leaving the
  scaffolding in place for a later phase rather than ripping it out.
- **Choice:** (b), for this pass.
- **Reason:** decomposing a playlist into N jobs touches job creation, queue behavior
  under #17's new priority ordering, per-item failure/retry UX, and progress reporting
  across N jobs -- a real feature, not a bug fix, and one this audit pass has no product
  sign-off to design unilaterally. The existing `WithPlaylistIndex`/playlist-count fields
  are cheap to keep parked (they cost nothing unused) so the day playlist support does
  ship, the numbering scheme doesn't need re-deriving from scratch.
- **Consequences:** a playlist URL reliably fails fast today (`E_PLAYLIST_NOT_SUPPORTED`)
  instead of silently downloading one entry or hanging on a full playlist walk. Playlist
  support remains a scoped, trackable future feature (issue #41 stays open for that) rather
  than a vague TODO.

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
- **Choice:** treat this as not applicable rather than build a placeholder or a
  Pro-locked stub for it. There is no raw-media copy anywhere in the pipeline to
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
  ("all rights reserved," matching the Pro-tier commercial model) applies by absence of a
  file, not by a considered choice — `docs/licensing.md` flags this explicitly so it isn't
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
