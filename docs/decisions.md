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
