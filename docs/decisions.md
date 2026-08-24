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

## Phase 5

### Building the missing conversion/compression layer, rather than a queue over one job type

Phase 5's specification describes Phases 1–4 as complete. The repository was at Phase 2:
`FFmpegEngine::Convert`/`Compress` threw `E_NOT_IMPLEMENTED`, `createJob` rejected
`CONVERSION` and `COMPRESSION`, and no `ConversionJob`, `CompressionJob`,
`FFmpegArgumentBuilder` or equivalent existed. `README.md` said so explicitly.

The phase's central requirement is a *unified* queue spanning downloads, conversions and
compressions, with a real download → convert → compress pipeline. A queue over one job type
cannot demonstrate any of that: no dependency chain, no mixed-type scheduling, no way to
show that concurrency caps real encoder processes.

So this phase built the minimum real version of that layer: a closed set of target formats
and compression presets, a pure `FFmpegArgumentBuilder`, real `Convert`/`Compress` with
progress, cancellation and atomic output, and `ConversionJob`/`CompressionJob` over a shared
`MediaProcessingJob` lifecycle.

Deliberately **not** built, because they are not needed for the queue to be real and belong
to the phase that owns compression properly: target-size compression (needs two-pass),
image conversion/compression, document conversion, `ExtractAudio`/`ExtractFrames`, and
per-codec tuning beyond the built-in recipes. Those remain declared-and-honest.

### `JobManager` evolved into the queue, rather than a queue layered over it

The specification suggests "one coherent orchestration layer over the existing
`JobManager`". Taken literally, that means a second structure holding jobs back and handing
them to `JobManager` only when eligible — which is two queues. `JobManager`'s own FIFO and
worker pool would become vestigial, jobs would be pending in one structure while another
also believed it owned them, and `GetJob`/`ListJobs` would have to merge two sources of
truth.

That is exactly the competing-queues problem the phase exists to prevent, so `JobManager`
was evolved in place instead. Its entire Phase 1 public surface still exists and behaves the
same way — every existing caller and test kept working — and the internals were replaced:
the FIFO deque became a priority/dependency-aware pending set in `SchedulerCore`, and the
fixed worker pool became one scheduler thread plus a worker per running job.

This is a deviation from the letter of the specification and satisfies its intent better.

### `SchedulerCore` is pure: no threads, no locks, no clock

Scheduling policy and thread management are separated absolutely. `SchedulerCore` decides
what runs next; `JobManager` runs it. Every `SchedulerCore` method that needs the current
time takes it as a parameter rather than reading a clock.

The payoff is that the entirety of this phase's scheduling behaviour — FIFO, priority,
concurrency admission, reordering, pause, dependency gating, retry eligibility, fairness
aging, history eviction — is testable as ordinary deterministic function calls. Its 48 tests
run in 2ms with no sleeps and nothing that can flake. The alternative, testing scheduling
through a live threaded manager, would have meant timing-dependent tests, and the
specification is explicit that arbitrary sleeps are not an acceptable way to make a test
pass.

### Concurrency enforced by admission, not by thread-pool size

Phase 1 capped concurrency by running exactly N worker threads that each pulled one job.
Simple, but the limit is then fixed at construction and is a cap on *job objects picked up*
rather than on work actually running.

Phase 5 caps it at the admission decision in `SelectDispatchable` and spawns a worker per
dispatched job. That makes the limit changeable at runtime, and makes it a cap on real
running processes — which is what the user means by "run 2 at once", and what the end-to-end
suite verifies by counting real `ffmpeg` children rather than trusting the job count.

### No `PAUSING` state

The specification's suggested state list includes `PAUSING`. It was not added, because there
is no asynchronous pause handshake in this architecture for a job to sit in: pause is either
cooperative and immediate (a job that checkpoints through `WaitWhilePaused`) or unsupported
for that job type. A state nothing can ever be observed in is worse than no state.

### Per-job pause is unsupported for media jobs, and says so

There is no reliable cross-platform way to suspend a running `yt-dlp` or `ffmpeg` process,
and the alternative — killing it and restarting later — is not pausing: it discards work and
would require resumable-output support neither tool is being asked for.

Rather than implement a "pause" that leaves a process consuming CPU and bandwidth while the
UI claims otherwise, `pauseJob` returns `E_JOB_INVALID_OPERATION` for these job types with a
message that says to cancel instead, and the UI disables the control. The specification is
explicit that a fake pause is worse than no pause.

Queue-level pause is real and means precisely "do not start additional work".

### Retry classification defaults to *not* retrying

Classification is allow-list shaped: an error is transient only when there is a specific
reason to believe a second attempt might differ. Everything unrecognized is permanent.

A retry that cannot succeed costs the user time, burns the budget, and buries the real error
under identical copies of itself. `DOWNLOAD_FAILURE` illustrates why the default matters —
it covers both "the connection dropped" and "this video is private", so without a specific
code distinguishing them, retrying is a coin flip that mostly loses.

Classification reads the structured `ErrorInfo` — category, then machine code — and never
free-text stderr, which would break the first time yt-dlp or FFmpeg reworded a message.

### `E_JOB_INTERRUPTED` is classified permanent

A job that was mid-flight when the process died is genuinely *unknown*, not permanent. But
the classifier has two buckets and the rule is that uncertainty means no automatic retry.

Auto-restarting these would re-download gigabytes without the user asking, or re-run an
encode over a partial file. The error is marked `recoverable`, so the UI offers Retry and the
decision stays with the user. This directly contradicted an earlier draft where the code was
listed as transient; a test caught the contradiction with the documented recovery policy.

### Restart recovery fails interrupted jobs rather than re-queueing them

`RUNNING` at crash → `FAILED(E_JOB_INTERRUPTED)`, not `QUEUED`.

We cannot tell what state the output is in: a half-written download or a killed encode leaves
bytes that may or may not be usable, and neither tool is being asked to resume. Re-queueing
silently risks enormous unrequested downloads, and — worse — could feed a truncated file to
the next stage of a pipeline as though it were valid. Failing loudly with a retryable error
is the honest option, and a manual retry sweeps artifacts first so it starts clean.

### No jitter in the retry backoff

Jitter exists to desynchronise many clients hammering one server. This is a single local
desktop app retrying its own handful of jobs; there is nothing to desynchronise from. It
would buy nothing and cost the exact determinism the retry tests depend on.

### Duplicate policy: reject and name the collision

Two pending requests are duplicates iff their identity keys are byte-identical (job type plus
canonical param serialization). On collision, `createJob` fails with `E_DUPLICATE_JOB` and
the existing job's id in `details`, so the UI can focus that job rather than quietly starting
a second identical download. `allowDuplicate: true` overrides it explicitly.

Anything less exact is treated as a different request. Merging merely-similar jobs silently
loses user intent — converting a file to MP3 and to WAV are two requests, not one.

### Event sequence numbers are stamped at the write point, not at construction

Originally the sequence was assigned where the event was created. Several threads publish
concurrently, so increasing numbers reached the wire out of order — which defeats the entire
purpose of having them. The end-to-end suite caught it.

The counter now lives in the stdout writer and is incremented under the same lock that
serializes lines, so sequence order and byte order on the wire are the same thing. As a
consequence sequence is *not* a field on `Event`: ordering is a property of the channel, not
of the event, and modelling it on `Event` was what allowed the bug.

### A pipeline stage names the job it reads from, not a path

`download → convert → compress` needs the second stage's input to be the first stage's
output, and that path is not knowable when the pipeline is declared: yt-dlp names the file
from the media's title and whichever container the extractor chose, and deduplication may
then have moved it to `Title (2).mp4`.

An earlier version had the frontend construct the expected path. It was wrong for exactly the
download case the feature exists for. A stage now sets `inputFromJobId`, the backend resolves
the real path immediately before the follower runs, and declaring it implies the dependency —
so a stage can neither start early nor run against a file that was never produced.

### Output names are reserved, not just deduplicated

Deduplication asks the disk whether a name is free. Two jobs that ask before either has
written both get "yes". Unreachable at Phase 1's concurrency of one; routine at Phase 5's,
and reproduced six times out of six: three downloads of the same title, all reporting
`COMPLETED`, one file on disk. Two of the user's downloads were silently destroyed.

`filesystem::OutputNameRegistry` reserves a name under a lock that spans choosing *and*
recording it, so two callers cannot settle on the same candidate. Reservations release on
scope exit, so a cancelled or failed job cannot leak a claim.

It is a process-wide singleton, which is not usually the right shape — but the resource it
guards, the set of names this application is about to write, is itself process-wide, and the
alternative was threading a new dependency through every job type, every constructor, and
every test for no behavioural gain.

### Artifact cleanup is keyed on what the job produced, not on the name it wants

`MediaProcessingJob` initially swept whatever sat at its desired output name before each
attempt, to stop retries accumulating `clip (1).mp3`, `clip (2).mp3`. That deletes a user's
unrelated `clip.mp3` on the very first attempt — real data loss, caught by a test.

The sweep now targets the exact path *this job* chose on its previous attempt, recorded
before the engine runs. A first attempt therefore deletes nothing and deduplicates around a
taken name; a retry still reclaims its own name.

### `vitest` for the frontend, and only for pure logic

`vite` is already the build tool, so `vitest` adds a dev dependency and no new concepts — no
jsdom, no component-rendering harness, no new config. The 57 tests cover the queue reducer
and the display/control-availability helpers, which is where the logic that can silently
break lives. Rendering is verified by running the real app.

The specification warns against introducing an enormous frontend testing framework for this
phase; this is the smallest thing that tests the parts worth testing.

### Portability fixes taken in this phase

Three Windows-only assumptions blocked building and testing the core on the Linux machines
used for development and CI. They are not Phase 5 features, but a phase cannot verify itself
against a suite that will not link:

- `WindowsHardwareDetector` had no definition off Windows, so the executable failed to link.
  It now has a POSIX fallback that reads `/proc/cpuinfo` and reports GPUs as none rather than
  inventing plausible-looking hardware.
- FFmpeg discovery shelled out to `where`. It now picks the host's lookup command — still one
  discovery path, it just knows which tool the host ships — and memoizes a successful answer,
  since the queue resolves paths far more often than Phase 2 did. A *failed* lookup is not
  cached, so installing ffmpeg mid-session works without restarting.
- `QueuePersistence::DefaultStateFilePath` concatenated a literal `\`, producing one
  absurdly-named file instead of a directory. It now builds the path through
  `std::filesystem`, which yields the same backslash path on Windows.

Fifteen tests that assert genuinely Windows-specific behaviour (backslash separators,
drive-letter roots, `cmd.exe`) now `SKIP` off-Windows instead of `FAIL`, via
`tests/support/PlatformTest.h`, so a Linux run gives a clean signal to detect real
regressions against.

### A failed handoff to a child process is transient

`WriteLine` on `RealProcessRunner` writes to the child's stdin from the caller's thread,
while the drain thread may be reaping that child in `wait()`. Reproc closes its handles
there, so a write landing in that window fails with `EINVAL`, not the `EPIPE` the code
tolerated — and `E_PROCESS_WRITE_FAILED` is not a category the classifier retries, so the
job failed permanently on a race. It hit roughly one download job in eight once Phase 5
started launching processes for retries and concurrent work.

Fixed at both levels, deliberately:

- `WriteLine` now tolerates the whole "the child is already gone" error class, not just
  broken pipe. Swallowing is correct rather than convenient here: a child that has exited
  cannot be diagnosed from the write, and the useful diagnosis — its exit code and whatever
  it printed — is exactly what `Wait()` is about to report. A command line that never
  arrived simply becomes the child's own "produced no result" error, which names the real
  problem. Anything that is not "the child is gone" still throws.
- A failed *handoff* (`E_PROCESS_WRITE_FAILED`, `E_PROCESS_START_FAILED`,
  `E_FFMPEG_LAUNCH_FAILED`, `E_FFPROBE_LAUNCH_FAILED`) is classified transient. The child
  never received its instructions, so nothing about the request has been shown to be wrong —
  and the attempt cost nothing, which makes one more try cheap.

The class comment claiming that *only* the drain thread ever touches the handle was also
corrected: it was true of wait/poll/read/terminate/kill and never true of stdin writes, and
a comment the code contradicts is worse than no comment.

## Phase 6

### Dark by default, unconditionally — not `prefers-color-scheme`

- **Context:** the spec asks for a modern, dark, premium product identity. The design
  system's first draft defined the dark palette on bare `:root` and a light palette under
  `@media (prefers-color-scheme: light)`, on the assumption that dark was the default and
  light was a considerate fallback for a user who had set their OS to light mode.
- **What actually happened:** verified by launching the real Tauri app under a virtual
  display (Xvfb, no desktop environment configured), which reports no dark preference by
  default — the same as most out-of-the-box desktop installs. The app rendered in light
  mode. Most desktops default to a light system theme, so `prefers-color-scheme` would have
  made light the *common* case in practice, the opposite of the product's identity.
- **Choice:** one palette, defined unconditionally, with no light variant. There is
  currently no in-app appearance setting (see "Settings scope" below — a toggle needs a
  real effect to exist), so there is nothing for a light palette to serve yet.
- **Consequences:** a user who prefers a light desktop gets a dark app anyway. If an
  appearance setting is ever added, it should drive an explicit choice (a settings field
  read at startup, the way the rest of Settings works), not an implicit OS media query.

### Evolved `JobManager` again, did not add a second frontend state layer

- **Context:** Phase 5 already made the mistake of a second orchestrator once, by nearly
  layering a queue controller over `JobManager` instead of evolving it. Phase 6 had the
  analogous frontend choice: `DownloaderPage` and `DevConsole` each held their own
  `useJobs()` polling hook, separate from the queue store `QueuePage` used
  (`state/useQueue.ts` / `queueReducer.ts`).
- **Choice:** deleted `hooks/useJobs.ts`. `App.tsx` now owns exactly one `useQueue()`
  instance and passes it to every screen that needs job data.
- **Reason:** two hooks meant two independent reconciliation paths that could show two
  different ideas of the same job's state — exactly the "duplicated state" failure mode the
  Phase 6 audit was asked to look for.
- **Consequences:** `DevConsole`'s self-test job and `DownloaderPage`'s active-download
  panel now update from the same event stream the Queue page does, so a job started on one
  screen is immediately visible, correctly, on another.

### Settings scope: only fields with a demonstrated real effect are editable

- **Context:** spec section 22 is explicit — "every setting must have a real backend
  effect" and "do not add settings for functionality that does not exist." `Settings.h` (six
  Phase-1-era categories, ~20 fields) already exists and round-trips through
  `getSettings`/`updateSettings`, so a plausible-looking Settings page was easy to build
  from the type alone.
- **What was actually checked:** grepped `app/core/main.cpp` and `core/` for every field's
  name to see whether anything besides `Settings.cpp`'s own serialization reads it.
  `processing.concurrentJobs` and `advanced.ffmpegPath` are read once, at `AppContext`
  construction, to size the job manager and build the FFmpeg engine. Nothing else —
  `defaultOutputDirectory`, `defaultQuality`, `downloadDirectory`, `filenameTemplate`,
  `hardwareAccelerationEnabled`, `defaultCompressionQuality`, `defaultOutputFormat`,
  `ytDlpPath`, `logLevel`, `launchOnStartup`, `crashReportingEnabled` — is read by anything
  other than the settings store itself.
- **Choice:** the Settings page exposes exactly three controls: "show notifications"
  (a real, immediate frontend effect — it gates `useQueueNotifications`), the FFmpeg path
  (real, applies next launch — labeled as such rather than implying it's immediate), and
  queue concurrency shown read-only with a link to the Queue screen's live control, rather
  than as a second control for the same value that `updateSettings` does not itself apply
  live (only `setConcurrency` does, and it writes back through the same setting). Everything
  else in `Settings.h` is not shown.
- **Consequences:** the Settings page looks sparse relative to what the type system
  suggests it could show. That is the honest state of the backend, not a frontend
  limitation — see `docs/roadmap.md` "UI" for wiring the rest up as real functionality
  lands behind each field.

### Developer console moved off primary navigation

- **Context:** `DevConsole.tsx` (Phase 1's IPC proving ground) was one of four top-level
  tabs in the old `App.tsx`. Spec section 4: "do not create navigation entries merely to
  fill space," and section 5 names Home/Download/Process/Queue/Settings as the product's
  actual concepts — a raw-JSON diagnostics screen is not one of them.
- **Choice:** kept the screen (it is still useful for exactly what it was built for — raw
  IPC inspection, drag-and-drop testing, settings/hardware round-trips) but moved it behind
  Settings → Developer, one click deeper, rather than deleting it or leaving it in the
  five-item primary shell.

### RTL added for the frontend suite, narrowly

- **Context:** Phase 5's `docs/decisions.md` entry for `vitest` deliberately chose "pure
  logic only, no rendering harness," reasoning that rendering was verified by running the
  real app. Phase 6 adds five real screens, a navigation shell, and a notification system
  whose entire job is reacting correctly to state transitions — logic that lives in how
  components render and respond to events, not in a standalone reducer.
- **Choice:** added `@testing-library/react`, `@testing-library/user-event`, and `jsdom` as
  dev dependencies, scoped to the specific components where rendering behavior is the thing
  being tested (`AppShell`, `QueuePage`, `useQueueNotifications`) — not a blanket policy to
  snapshot-test every component.
- **Consequences:** rendering is still also verified by running the real app (Phase 6 did
  this via a virtual display and screenshots — see `docs/phase-6.md`); the RTL tests catch
  a narrower, faster-running class of regression (a control's disabled state, a toast firing
  on the wrong transition) that a full app launch would not conveniently assert against.
