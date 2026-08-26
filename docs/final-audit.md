# Gravity — Final Independent Audit

**Date:** 2026-08-24
**Commit audited:** `f410ec2` (branch `claude/gravity-complete-audit-lvz5ig`, identical to `master` at time of audit)
**Audit host:** Linux x86-64 (see [Environment limitations](#18-environment-limitations) — **no Windows host was available**)
**Scope:** the entire repository — C++ core, engines, Python downloader, Rust/Tauri shell, React frontend, build system, tests, packaging, documentation.

This audit treats the claim that Gravity is "release ready" as unverified. Every conclusion
below is derived from reading the code and from independently running the builds, the test
suites and purpose-built reproduction harnesses. Where something could not be verified, it
is recorded as **NOT VERIFIED**, never as passing.

---

## 1. Executive summary

**Verdict: NOT RELEASE READY.**

Two independent reasons, either of which is sufficient on its own.

**First — it cannot be shipped.** `tauri build` produces an installer containing the Rust
shell and the web assets and nothing else. The C++ core, the Python downloader, yt-dlp and
FFmpeg are all resolved at runtime from paths that only exist in a developer's source tree
(`../../build/windows-mingw-debug/...`, `python/downloader/.venv/Scripts/python.exe`). An
installed Gravity fails on the user's first action. This is already reported as issue #1 by
the repository owner; the audit identifies it as a packaging gap, not a bug
([#7](https://github.com/h-kakar11/gravity/issues/7)). Separately, there is no `LICENSE`
and no third-party attribution, and the recommended FFmpeg build is GPL — so no installer
can lawfully be published until that is resolved
([#30](https://github.com/h-kakar11/gravity/issues/30)).

**Second — a confirmed, reproducible data-loss defect.** A failed or cancelled download
permanently deletes pre-existing files *and entire directory trees* in the user's output
folder that merely share a name prefix with the video title. Reproduced against the real
`DownloadJob` and the real filesystem: four pre-existing user files destroyed by one failed
download ([#3](https://github.com/h-kakar11/gravity/issues/3)). The design note in
`docs/decisions.md` that argues this is safe is incorrect.

Beyond those, three further confirmed defects were reproduced: a cancel/start race that
aborts the entire core process via `std::terminate`
([#4](https://github.com/h-kakar11/gravity/issues/4)); a single `updateSettings` call that
permanently bricks the application on every subsequent launch
([#5](https://github.com/h-kakar11/gravity/issues/5)); and a shutdown path that *starts*
queued jobs after shutdown has begun and blocks until they all finish
([#6](https://github.com/h-kakar11/gravity/issues/6)).

**A separate and larger point about scope.** The repository is at the end of Phase 2 of a
ten-phase plan, and its own documentation says so honestly. The product Gravity is intended
to be — Home / Download / Convert & Compress / Queue / Settings, dark and polished, with a
unified queue supporting concurrency, priorities, retries, dependencies and persistence —
does not substantially exist. Convert and Compress throw `E_NOT_IMPLEMENTED` at every entry
point. There is no Queue screen, no Settings screen, no navigation shell, no dark theme, and
no persistence of any kind. The architectural components named in the brief as existing
principles — `SchedulerCore`, `MediaProcessor`, dependency-aware jobs, persistent queue
state — are not present in the codebase at all.

**What is genuinely good.** The download vertical slice is real and works. The layering is
clean and the interface seams are in the right places. Subprocess execution is argv-based
throughout with no shell string construction anywhere — the single most common security
defect in this class of application is simply absent. `docs/decisions.md` is an unusually
honest record of real trade-offs. The 146 C++ tests and 24 Python tests pass. There is a
sound foundation here; it is roughly a third of a product, and it is currently unshippable.

**Path to release, in order:** #3, #7, #30, #5, #4, #6 → then decide whether Gravity 1.0 is
a downloader (in which case #14, #16, #23, #8, #9, #13 and honest documentation) or the full
product (add #15, #10, #17).

---

## 2. Product understanding

Gravity is intended as a local-first Windows media utility: download from URLs, convert,
compress, all through one unified queue, with FFmpeg and yt-dlp hidden behind a simple UI.
No accounts, no telemetry, no cloud. The organising principle is **simple UI, powerful local
engine**.

Sources consulted: `README.md`, `docs/architecture.md`, `docs/roadmap.md`,
`docs/decisions.md`, `docs/ipc-contract.md`, `docs/protocols/downloader.md`,
`docs/phase-2.md`, and `idealist.md` (the product-vision brain-dump at repository root).

Mapping the 20 stated product intents against the implementation:

| # | Intent | State |
|---|---|---|
| 1 | Download media from URLs (esp. YouTube) | **Working** |
| 2 | Inspect a URL before downloading | **Working** (blocks the whole backend, #8) |
| 3 | Select available formats and quality | **Partial** — presets work; format list fetched then discarded (#31) |
| 4 | Download video / audio / video+audio | **Working** (`AUDIO_ONLY` can silently yield video, #31) |
| 5 | Real-time progress, speed, ETA | **Working** |
| 6 | Cancel jobs safely | **Partial** — cancels, but can crash the process (#4) and orphans children (#9) |
| 7 | Convert local media | **Not implemented** (#15) |
| 8 | Compress with presets | **Not implemented** (#15) |
| 9 | Download → convert → compress pipelines | **Not implemented** (#15, #17) |
| 10 | One unified queue | **Backend only** — no UI (#16) |
| 11 | Concurrency, priorities, retries, dependencies | **Concurrency only**; the other three absent (#17) |
| 12 | Persist queue across restarts | **Not implemented** (#10) |
| 13 | Recover after crashes | **Not implemented** (#10) |
| 14 | Prevent collisions and data loss | **Actively violated** (#3, #12) |
| 15 | Use FFmpeg locally | **Working for probing**; not bundled (#7); processing absent (#15) |
| 16 | Use yt-dlp locally | **Working**; not bundled (#7) |
| 17 | No cloud, accounts, telemetry | **Held** — one caveat: remote thumbnails (#28) |
| 18 | Clear diagnostics / version info | **Not surfaced** — backend can report it, no UI does (#16, #20) |
| 19 | Fast, reliable, professional | **Not yet** — see reliability findings |
| 20 | Hide FFmpeg/yt-dlp complexity | **Held** — `QualityPreset` is a genuinely clean abstraction |

**8 of 20 fully met, 5 partial, 7 absent.**

---

## 3. Architecture assessment

The architecture is the strongest part of this repository. Process topology:

```
React / TypeScript  --Tauri IPC-->  Rust shell  --stdio NDJSON-->  C++ core (mediatool-core)
                                                                          |
                                                             stdio NDJSON |
                                                                          v
                                                          Python downloader -> yt-dlp -> ffmpeg
```

### Principles that hold

- **One unified JobManager, no competing queues.** Verified: `DownloadJob` is a `Job` on
  the same worker pool as `TestJob`. There is no downloader-specific queue.
- **Real subprocess execution, argv-based throughout.** `IProcessRunner` takes a structured
  `vector<string>`; `reproc` is given argv, never a command string. No `system()`, no
  `popen`, no `ShellExecute`, no `cmd /c` in any production path.
- **One quality vocabulary above the engines.** `QualityPreset` is the only vocabulary
  above `engines/downloader`; `YtDlpFormatSelector` is the single translation point. This
  is exactly right.
- **NDJSON IPC with one framing convention across both hops.** Correctly implemented.
- **Clean interface seams.** `IProcessRunner`, `IMediaEngine`, `IDownloadProvider`,
  `IFileSystem`, `IClock` — production depends on interfaces, tests on mocks.
- **Structured events, not log-scraping.** `EventBus` publishes typed events; the frontend
  never parses log text.

### Principles that do not hold

- **`SchedulerCore` does not exist.** Scheduling is `queue_.front()` on a `std::deque`
  inside `JobManager`. No priority, no dependency graph, no separation of decision from
  execution (#17).
- **`MediaProcessor` does not exist.** `IMediaEngine` mixes discovery/probing (implemented)
  with processing (all four methods throw). The intended split has not been made (#15).
- **"One FFmpeg discovery path" is true structurally but not operationally.**
  `FFmpegEngine` re-runs `where` on every call while `main.cpp` resolves once at startup —
  two discovery lifetimes for one authority (#20).
- **Persistence does not exist** (#10). `AtomicWriter` and `TempDirectory` — the two
  primitives written for it — are unused, while `docs/architecture.md` describes them as
  active.
- **`IFileSystem` is wider than its use.** Four of eight methods have no production caller,
  and `MockFileSystem` diverges from `LocalFileSystem` on `Delete` semantics — which is
  precisely what allowed #3 through the test suite (#39).

### Cross-cutting observations

- **Abstraction leakage:** none significant found. Notably, the frontend contains zero
  media logic and never touches a subprocess.
- **Circular dependencies:** none. Layering (`core` ← `engines` ← `app`) is respected.
- **Threading model:** documented per-class and mostly correct. Lock ordering is consistent
  (callbacks are always fired outside the holder's lock, avoiding the obvious deadlock).
  Two real defects found: #4 (unhandled exception in a worker thread) and #36 (raw `Job*`
  used outside the owning lock — latent, not currently reachable).
- **Duplicated responsibility:** output-directory creation happens in both `DownloadJob`
  and `downloader.py`; two settings pairs describe one behaviour each (#18).

---

## 4. Functional audit

### Download

| Capability | State |
|---|---|
| URL validation | Working — http/https prefix check; `file://`, `ftp://`, garbage all rejected cleanly (verified) |
| Playlist handling | Deliberately rejected, fast and cleanly (#41 — correct as implemented) |
| Inspection / format enumeration | Working; blocks the IPC loop, uncancellable, no timeout (#8) |
| Quality selection | Working; preset can silently substitute a different stream (#31) |
| Audio-only | Working; `bestaudio/best` fallback can yield a video file (#31) |
| Video+audio merge | Delegated to yt-dlp's ffmpeg, pointed at the resolved binary — good decision |
| Output naming / collisions | Working for the sequential case; TOCTOU under concurrency (#12) |
| Reserved names / long paths | **Broken** — `CON`, `NUL`, `COM1` unhandled; no MAX_PATH check (#13) |
| Progress / speed / ETA | Working |
| Cancellation | Works, but can crash the process (#4) and orphans grandchildren (#9) |
| Cleanup after failure | **Destroys user data** (#3) |
| Output verification | Working — existence, non-zero size, ffprobe round-trip before `COMPLETED`. Good. |
| Retries | Manual only; no policy, `ErrorInfo.recoverable` computed and never read (#17) |
| Reported output path | Derived from `prepare_filename()`; can be wrong after a merge (#25, unverified) |

### Conversion / Compression

**Not implemented at any layer.** `FFmpegEngine::Convert/Compress/ExtractAudio/ExtractFrames`
all throw `E_NOT_IMPLEMENTED`; `createJob` rejects `CONVERSION`/`COMPRESSION`/`BATCH`/`WORKFLOW`;
no UI. Every sub-item in the brief's conversion and compression checklists is therefore
**not applicable**. `getCapabilities` nonetheless advertises `convert`, `compress`,
`extractAudio`, `extractFrames`, `resize`, `convertToText` and `convertToHtml` — a live
contract defect independent of when the feature lands (#15).

### Queue

| Capability | State |
|---|---|
| Multiple job types | Only `DOWNLOAD` and `TEST` are runnable |
| Concurrency | Supported by the pool; default 1; unsafe to raise until #12 is fixed |
| Priority | **Absent** (#17) |
| Retries | Manual only (#17) |
| Dependencies | **Absent** (#17) |
| State transitions | `JobStateMachine` is correct and well-tested |
| Persistence / restart recovery | **Absent** (#10) |
| Shutdown | **Broken** (#6) |
| Deadlocks | None found; lock ordering is consistent |
| Races | One confirmed (#4), one latent (#36), one TOCTOU (#12) |
| Job removal | Implemented, unreachable from IPC; unbounded growth (#29) |

---

## 5. Security audit

Adversarial review of untrusted input from IPC/UI through to subprocess arguments and
filesystem operations, plus a live fuzz of the real `mediatool-core` NDJSON loop.

### What is right

**Command and argument injection: not present.** Every subprocess launch in the codebase
was traced. `IProcessRunner::Start(executable, vector<string> args, ...)` passes argv to
reproc; no shell is invoked anywhere. The URL — the most obviously attacker-controlled
input — never becomes a command-line argument at all: it is carried as a **JSON field** on
`downloader.py`'s stdin and consumed by yt-dlp's Python API. This is the correct design and
it eliminates the highest-severity risk class for this kind of application.

The Rust shell's one external invocation (`explorer.exe`) also avoids the shell, though its
argument is interpolated into a string that Explorer parses itself, contrary to its comment
(#38, low).

**Privacy claims hold**, with one caveat: no telemetry, no analytics, no cloud upload, no
network calls beyond the downloader itself — except that the Download page loads thumbnails
directly from the video host, disclosing the user's IP on inspect (#28).

### Findings

| Finding | Severity | Issue |
|---|---|---|
| `updateSettings` unvalidated → persistent DoS, app bricked on every launch | High | [#5](https://github.com/h-kakar11/gravity/issues/5) |
| Output directory unvalidated — relative, `../` traversal and UNC all accepted | Medium | [#11](https://github.com/h-kakar11/gravity/issues/11) |
| CSP disabled (`"csp": null`) + remote images in a page with full IPC privilege | Medium | [#28](https://github.com/h-kakar11/gravity/issues/28) |
| Prefix-match delete + recursive `remove_all` destroys user data | Critical | [#3](https://github.com/h-kakar11/gravity/issues/3) |
| `explorer.exe` argument built by interpolation; misleading safety comment | Low | [#38](https://github.com/h-kakar11/gravity/issues/38) |
| Dev-dependency advisories (vite, esbuild); no update process | Low | [#40](https://github.com/h-kakar11/gravity/issues/40) |

### Fuzz results (real binary, adversarial NDJSON)

23 hostile inputs were fed to the real `mediatool-core`. **No memory-safety crash occurred**
and no input produced non-JSON on stdout (the protocol held). Notable results:

- Handled correctly: unknown command, bogus job type, `file://` URL, invalid quality enum,
  unknown job id, empty output directory — all clean, specific error codes.
- Handled poorly: missing/wrong-typed parameters → `E_UNHANDLED_EXCEPTION` with raw
  nlohmann text; malformed `id` → an unroutable empty-id response (#21).
- Accepted when it should not be: traversal and UNC output directories (#11);
  `concurrentJobs: 100000` (#5); `analyticsEnabled: true` despite being documented as
  immutable (#5).
- `inspectFile` on `/etc/shadow` returned metadata — arbitrary filesystem stat from the
  webview (#11).
- Unbounded input: `std::getline` on stdin has no length cap; a 1 MB URL was accepted
  (#21). Not exploitable through the trusted Rust bridge, but unbounded by design.

Not assessed: malicious *media* files. FFmpeg's own parsing surface is out of scope for
this audit, but note that Gravity currently runs ffprobe against downloaded content with no
sandboxing — worth a decision if untrusted local files become an input path.

---

## 6. Reliability / concurrency audit

Four reproduction harnesses were built against the real classes. Three confirmed defects.

### Confirmed: cancel/start race aborts the process (#4)

`RunJob` reads `State() == Queued`, then calls `MarkStarting()`. A `cancelJob` in that
window transitions to `Cancelled` first, so `MarkStarting()` throws — and `WorkerLoop` has
no try/catch, so the exception escapes the thread function.

```
terminate called after throwing an instance of 'mediatool::errors::MediaToolException'
  what():  Internal error: invalid job state transition
Aborted (exit 134)
```

Three runs aborted at ~1,200 / ~3,400 / ~6,800 submit-cancel cycles. Consequence: total
loss of every in-flight job, with no persistence to recover from.

### Confirmed: shutdown starts new work and blocks (#6)

`JobManager(1)`, six 400 ms jobs, one already running at destruction:

```
shutdown returned after 2401 ms
jobs started=6 finished=6, of which STARTED AFTER shutdown began=5
```

### Confirmed: settings value bricks the app (#5)

One `updateSettings` call, then a restart:

```
terminate called after throwing an instance of 'std::system_error'
  what():  Resource temporarily unavailable
Aborted (exit 134)
```

`main()` has no top-level exception handler, so there is no diagnostic at all.

### Confirmed: cleanup destroys user data (#3)

Real `LocalFileSystem`, real `DownloadJob`, simulated mid-download network failure:

```
BEFORE: Song (1).mp4, Song - Live at Wembley.mp4, Song Collection/{track01,track02}.flac, Unrelated.mp4
AFTER:  Unrelated.mp4
RESULT: 4 pre-existing user file(s) destroyed
```

### Assessed against the brief's stress scenarios

| Scenario | Assessment |
|---|---|
| 1 / 10 / 100 jobs | Runs; unbounded queue and unbounded job retention (#29) |
| Duplicate / same-title jobs | TOCTOU on output naming once concurrency > 1 (#12) |
| Simultaneous cancellation | **Crashes** (#4) |
| Cancellation during startup | **Crashes** — this is the exact race (#4) |
| Cancellation during shutdown | Not handled — shutdown does not cancel anything (#6) |
| Failure during retry | Retry works; stale `error_`/`progress_` persist into the retried run (#17) |
| Dependency failure/cancellation | N/A — no dependency model (#17) |
| Process / parent crash | No recovery; queue lost (#10); orphan children survive (#9) |
| Machine restart | No state to restore (#10) |
| Corrupted / partial state | Corrupt settings file aborts every launch (#5) |
| Missing / externally deleted output | Detected — `E_DOWNLOAD_OUTPUT_MISSING`, then triggers the destructive cleanup (#3) |
| Deadlocks / livelocks | None found; lock ordering is consistent and callbacks fire outside locks |
| Starvation | FIFO only; a long job blocks everything behind it at concurrency 1 (#17) |
| Orphan processes | **Confirmed by design gap** — no tree termination anywhere (#9) |
| Orphan temp files | `.part` files survive a hard kill; nothing reconciles them (#10) |

---

## 7. Windows audit

**This is the weakest part of the verification, and it must not be overstated.** The audit
host was Linux. No Windows machine, VM or CI runner was available. Everything below is
derived from reading the code; nothing marked Windows-specific was observed running.

| Area | Assessment | Verified? |
|---|---|---|
| Illegal characters in filenames | Handled correctly | Code + unit tests |
| Reserved device names (`CON`, `NUL`, `COM1`…) | **Not handled** (#13) | Reproduced against the real sanitizer |
| MAX_PATH / long paths | **Not handled**; no long-path opt-in (#13) | Code + length arithmetic |
| Unicode paths | Handled — UTF-8-aware truncation that will not split a sequence. Good. | Code review |
| Drive letters, spaces in paths | Handled via `std::filesystem` | Code review |
| UNC paths | **Accepted where they should be rejected** (#11) | Reproduced via IPC |
| CWD assumptions | **Pervasive and known-broken** — core path, Python path, script path (#7) | Code; matches reported #1 |
| AppData paths | Used, but with hardcoded `\\` string concatenation rather than `std::filesystem` — contradicts the architecture doc | Code review |
| Executable discovery | `where`-based, Windows-only, uncached, override unverified (#20) | Code + unit tests |
| Sidecar discovery | Dev-only guess; no packaged resolution (#7) | Code review |
| Process termination | Single-process only; no Job Object (#9) | Code; corroborated by the repo's own test comment |
| Process trees / orphans | **Not handled** (#9) | Code review — **NOT VERIFIED on Windows** |
| Console window creation | No `CREATE_NO_WINDOW`; core is console-subsystem (#37) | Code review — **NOT VERIFIED on Windows** |
| Tauri sidecar behaviour | Not configured at all (#7) | Config review |
| Installer / upgrade / uninstall | **NOT VERIFIED — no installer could be produced** | — |
| UAC / permissions | No elevation assumptions found; per-user install implied but untested | Code review |
| DPI scaling 100/125/150/200% | **NOT VERIFIED — ENVIRONMENT LIMITATION** | — |

**Linux verification is explicitly not Windows verification.** The 134 passing C++ tests
ran on Linux with a stubbed hardware detector; the 12 that failed are Windows-path and
`cmd.exe`/`ping` assumptions. The Rust build succeeded for the GNU/Linux target, **not** for
the MinGW Windows target the project actually ships.

---

## 8. Packaging / release audit

Using the brief's five-way classification:

| Item | Classification |
|---|---|
| C++ core builds | **Implemented and tested** (on Linux with a stub; Windows/MinGW NOT VERIFIED here) |
| Frontend builds (`tsc` + `vite build`) | **Implemented and tested** |
| Rust shell builds | **Implemented and tested** (Linux/GNU target only) |
| Sidecar bundling | **Not implemented** (#7) |
| Python/yt-dlp bundling | **Not implemented** (#7) |
| FFmpeg bundling | **Not implemented** (#7) |
| Resource-relative path resolution | **Not implemented** (#7) |
| Installer (NSIS) configuration | **Implemented but not tested** — `targets: ["nsis"]` is set; no installer was ever produced or run |
| Icons | **Implemented** — four sizes present |
| Version consistency | **Not implemented** — 0.1.0 duplicated across five manifests with no sync check |
| Signing | **Not implemented** — no configuration of any kind |
| Update mechanism | **Not implemented** — and correctly not advertised anywhere |
| Clean install / reinstall / uninstall / upgrade | **NOT VERIFIED — no installable artifact exists** |
| Stale files after uninstall | **NOT VERIFIED** |
| Architecture (x64/ARM) | x64 implied throughout; ARM not considered. Deferred by omission, not by decision. |
| CI/CD | **Not implemented** — no `.github/` directory at all (#26) |
| Reproducible builds | **Not implemented** — `Cargo.lock` gitignored, no vcpkg baseline, unpinned `yt-dlp` (#7, #40) |
| LICENSE / third-party notices | **Not implemented** — a hard legal blocker (#30) |

The recommended FFmpeg source in `docs/development.md` (`Gyan.FFmpeg`) is a GPL build.
Bundling it alongside a proprietary application in one installer is exactly the case the
GPL governs. The architecture — invoking FFmpeg as a separate process — preserves the LGPL
option, but the decision has not been made or recorded (#30).

---

## 9. UI / UX audit

**The application could not be launched** (no Windows host, no display server, and the
Tauri shell cannot find its sidecar). No screenshots were taken. `docs/phase-2.md` records
that the previous session also failed to capture a click-through, so **the running UI has
never been visually verified by any pass.**

The assessment below is from source.

**What exists:** `App.tsx` is a two-button tab switch between `DownloaderPage` and
`DevConsole`. There is no Home, no Convert & Compress, no Queue, no Settings, no About, no
router, and no CSS file anywhere in the repository — every style is an inline
`CSSProperties` object in a **light** palette (`#fafafa` cards, `#ddd` borders). The
intended dark, restrained, professional application does not exist (#16).

| Check | Assessment |
|---|---|
| Expected navigation (Home/Download/Convert/Queue/Settings) | **Absent** — two tabs (#16) |
| Dark mode | **Absent** — light-only inline styles (#16) |
| Visual consistency / hierarchy | Serviceable for a dev console; no design system (#16) |
| Loading states | Text-only ("Inspecting..."); no skeleton; no cancel (#33) |
| Empty states | **Absent** (#33) |
| Error states | Present, but render raw Python tracebacks (#33) |
| Success / cancellation states | Present and reasonable |
| Progress states | Present; merge phase sits at 100% with no feedback (#33) |
| Disabled states | Present; colour-only, no reason given (#32) |
| Keyboard accessibility | No Enter-to-submit; focus lost on the primary action (#32) |
| Focus behaviour / tab order | No management at all (#32) |
| Long filenames / URLs / metadata | No overflow handling — will break layout (#33) |
| Small windows, resize, DPI | **NOT VERIFIED — ENVIRONMENT LIMITATION** |
| Technical jargon in the UI | Yes — "Phase 2 vertical slice", "spec section 34", raw job ids (#33) |
| Destructive actions | None exposed in the UI (the destructive behaviour in #3 is invisible to the user) |

**Backend/UI capability mismatches found:**

- 40+ selectable formats are fetched and only counted (#31).
- `retryJob`, `pauseJob`, `resumeJob`, `removeJob`, `getSettings`, `updateSettings`,
  `getHardwareInfo`, `getCapabilities` are all implemented and reachable in the backend and
  exposed nowhere in the UI (#16).
- The UI subscribes to `jobQueued`, which is never emitted (#22).
- `getCapabilities` advertises seven operations that all fail (#15).
- Only one job is trackable at a time; starting a second replaces the first (#16).

---

## 10. Accessibility audit

Assessed from source; no assistive technology was run (**NOT VERIFIED** behaviourally).

| Check | Result |
|---|---|
| Semantic controls | Native `button`/`input`/`select` used — a good baseline |
| Progress information | `role="progressbar"` / `aria-value*` **absent** — progress is two empty divs to a screen reader |
| Status announcements | No `aria-live` anywhere; state changes are silent |
| Error identification | No `role="alert"` on error banners |
| Keyboard navigation | No Enter-to-submit; no `form` element |
| Focus visibility / management | Browser default only; focus is lost when Download is replaced by Cancel |
| Labels | Present for quality and output folder; the URL input has a placeholder but no label |
| Contrast | `#888` on `#fafafa` ≈ 2.9:1 — **below WCAG AA 4.5:1**, and it carries the output path and metadata |
| Reliance on colour alone | Success/error distinguished by colour; no icon or severity text |
| Disabled-state clarity | Visual only, no reason given |
| Text truncation / scalable UI | No overflow handling (#33); DPI scaling NOT VERIFIED |
| Automated a11y linting | None — no ESLint at all |

Filed as [#32](https://github.com/h-kakar11/gravity/issues/32). Most of this should be
built into the UI work in #16 rather than retrofitted onto a page being replaced.

---

## 11. Performance audit

Only findings with credible, traceable impact are filed. Speculative micro-optimisations
were deliberately excluded.

| Finding | Impact | Issue |
|---|---|---|
| A `getJob` IPC round-trip per `jobProgress` event, at 10–50 events/s, through a single-threaded loop — plus out-of-order responses that rewind the progress bar | Real; scales with concurrent jobs; has a visible symptom | [#19](https://github.com/h-kakar11/gravity/issues/19) |
| FFmpeg discovery re-spawns `where` on every call — 4 process launches per file inspection | Tens–hundreds of ms per download on Windows; scales with batch size | [#20](https://github.com/h-kakar11/gravity/issues/20) |
| Three full metadata extractions and two Python launches per download | 3–6 s before the first byte; triples request rate against the host | [#35](https://github.com/h-kakar11/gravity/issues/35) |
| Unbounded job and event-state retention | Grows with session history; matters at Queue-screen scale | [#29](https://github.com/h-kakar11/gravity/issues/29) |
| Blocking `inspectDownloadUrl` on the only IPC thread | Freezes the whole backend | [#8](https://github.com/h-kakar11/gravity/issues/8) |
| Unbounded `LineSplitter` buffer; quadratic `erase(0,n)` per line | Latent | [#24](https://github.com/h-kakar11/gravity/issues/24) |

**Explicitly not filed:** `EventBus::Publish` copying its subscriber vector per event (one
subscriber in practice); `JobSnapshot` construction from nine getters (correct as a
deliberate simplification, documented); the sidecar process hop (justified in
`docs/decisions.md` and correct for operations measured in seconds).

**Not measured:** startup time, memory growth over a long session, large-queue behaviour —
all require a Windows host.

---

## 12. Testing audit

### Independently re-run — exact results

| Suite | Command | Result |
|---|---|---|
| C++ GoogleTest | `ctest` (Linux, patched hardware-detector stub) | **134/146 passed, 12 failed** |
| Python `unittest` | `python3 -m unittest discover -s tests/python -v` | **24/24 passed** in 0.271 s |
| TypeScript typecheck | `npx tsc --noEmit` | **clean** |
| Frontend build | `npx vite build` | **succeeded** — 43 modules, 175.68 kB |
| Rust build | `cargo build` (Linux/GNU) | **succeeded** |
| Rust lint | `cargo clippy` | **clean** |
| Rust tests | `cargo test` | **0 tests exist** |
| Frontend tests | — | **no framework, no test script, 0 tests** |
| npm audit | `npm audit` | 1 high, 1 moderate (both dev-only) |
| C++ build (MinGW/Windows) | — | **NOT VERIFIED — ENVIRONMENT LIMITATION** |
| `tauri build` / installer | — | **NOT VERIFIED — ENVIRONMENT LIMITATION** |
| `--selftest` (full) | — | **PARTIAL** — binary built and ran; ffmpeg and yt-dlp absent on the host |
| IPC fuzzing | custom harness, real binary, 23 hostile inputs | **no crash; 3 issues found** (#5, #11, #21) |

The 12 C++ failures are **not defects** — they are Windows-only assumptions:
`PathUtilsTest` ×8 (Windows separators, drive letters), `RealProcessRunnerTest` ×2
(`cmd.exe`, `ping`), `DownloadJob` ×2 (`C:\out` literals). The suite is genuinely green on
its target platform.

**The C++ core does not link on non-Windows** — `WindowsHardwareDetector` is declared
unconditionally but defined only under `#ifdef _WIN32`. A one-file stub was enough to build
and run the suite, so the gap is narrow (#26).

### Coverage assessment per capability

| Capability | Impl | Unit | Integration | E2E | Windows | Real deps |
|---|---|---|---|---|---|---|
| Job state machine | ✅ | ✅ | — | — | ❓ | n/a |
| JobManager lifecycle | ✅ | ⚠️ 3 tests | ❌ | ❌ | ❓ | n/a |
| JobManager concurrency/races | ✅ | ❌ | ❌ | ❌ | ❌ | n/a |
| DownloadJob | ✅ | ✅ 7 tests | ❌ | ❌ | ❓ | ❌ |
| Filename sanitize/dedup | ✅ | ✅ 14 tests | — | — | ❓ | ✅ |
| Cleanup after failure | ✅ | ⚠️ unrealistic mock | ❌ | ❌ | ❌ | ❌ |
| YtDlpProvider | ✅ | ✅ 11 tests | ❌ | ❌ | ❓ | ❌ |
| Python protocol | ✅ | ✅ 24 tests | ✅ subprocess | ❌ | ❓ | ❌ |
| FFmpeg probe | ✅ | ✅ 10 tests | ❌ | ❌ | ❓ | ❌ |
| IPC loop | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ |
| Rust bridge | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ |
| Frontend | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ |
| Packaging/installer | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |

### False-confidence tests found

- **`MockFileSystem::Delete` is non-recursive; `LocalFileSystem::Delete` is `remove_all`.**
  This divergence is exactly what let #3 pass the suite — the mock cannot represent the
  destructive case.
- **`DownloadFailureThrowsAndCleansUpArtifacts` seeds only the job's own artifact**, so it
  tests the one arrangement in which #3 does not manifest.
- **`ProcessRunnerTest` explicitly avoids the parent-with-children shape** *because killing
  the parent leaves the child alive* — documenting the exact hazard (#9) that production
  relies on, and then not testing it.
- **No test asserts on file contents**, only on recorded paths — so `DownloadJob`'s
  verification logic is exercised against a fiction.
- **The Phase 1 stdout/NDJSON corruption bug is still untested.** `docs/decisions.md`
  records that it survived a whole phase because nothing runs the IPC loop as a subprocess.
  Nothing does now either.

Filed as [#27](https://github.com/h-kakar11/gravity/issues/27) (coverage) and
[#26](https://github.com/h-kakar11/gravity/issues/26) (CI).

---

## 13. Documentation audit

The documentation is better than typical for a project this size. `docs/decisions.md` in
particular records genuine trade-offs with context, options and consequences — including
one entry that is an honest post-mortem of a bug the author introduced. `README.md`'s status
table makes a real effort to separate "working" from "scaffolded". Both are commendable and
should be preserved.

The problems are specific:

| Document | Finding |
|---|---|
| `README.md` | Titled `# MediaTool ("gravity")` — reads as though the product is MediaTool. Does not state what is missing relative to the intended product. |
| `docs/architecture.md` | Claims `TempDirectory` isolates each job's work and `AtomicOutput` protects outputs from crashes. **Neither is used in production.** The class is also named `AtomicWriter`. (#34) |
| `docs/decisions.md` | The cleanup-safety argument is **incorrect** — it conflates exact-stem with prefix matching, and is the reasoning that produced #3. (#3, #34) |
| `docs/ipc-contract.md` | Documents four events that are never emitted, and a `getCapabilities` vocabulary nothing implements. (#22, #15) |
| `docs/development.md` | Quick start runs `cmake --preset` before mentioning the required vcpkg bootstrap; promises a `scripts/dev.ps1` that does not exist; no CI, release or packaging guidance; Windows-only with no note that the code will not link elsewhere. (#34) |
| `docs/protocols/downloader.md` | Accurate and thorough. No findings. |
| `docs/phase-2.md` | Honest, including about the ungathered GUI verification. Its "Recommended Phase 3" list matches this audit's own priorities closely. |
| `docs/roadmap.md` | Accurate as far as it goes; covers a fraction of `idealist.md`. |
| `idealist.md` | Unstructured brain-dump at repo root; the de-facto product vision with no status, priority or reconciliation against the roadmap. (#34) |
| Missing entirely | `LICENSE`, third-party notices (#30), `release.md`, feature-freeze doc, `CONTRIBUTING.md`, phase-3+ docs |

**On the "release ready" claim:** no document in this repository actually makes it.
`README.md`, `docs/roadmap.md` and `docs/phase-2.md` are all clear that this is a Phase 2
vertical slice with conversion, compression, the queue UI and the final UI unbuilt. The
claim the audit was asked to test is not one the documentation supports — the docs' failure
is one of omission (never stating the size of the remaining gap), not of false assertion.

---

## 14. Existing strengths

Worth recording explicitly, because a defect list is not a fair picture on its own.

1. **No shell execution anywhere.** Every subprocess is argv-based. The URL never becomes a
   command-line argument. This eliminates the dominant vulnerability class for this kind of
   application, and it was clearly deliberate.
2. **Clean layering with real seams.** Five interfaces, mocks for each, production depending
   on abstractions. The frontend contains zero media logic.
3. **`QualityPreset`** is a genuinely well-executed abstraction — one vocabulary above the
   engine, one translation point, unit-testable without a process.
4. **Output verification before success.** A download is `COMPLETED` only after existence,
   non-zero size and an ffprobe round-trip. The instinct to not trust "exit code 0" is right.
5. **Structured errors end to end.** `ErrorInfo` with code, category, message, details and
   `recoverable` flows from Python classification through C++ to typed TypeScript.
6. **The `RealProcessRunner` race fix.** A real reproc concurrency hazard was found,
   diagnosed and fixed with an accurate 12-line comment explaining why. That is good
   engineering.
7. **`docs/decisions.md`.** Genuinely useful, including recording a mistake as a decision
   entry so a future change does not undo the fix.
8. **Playlist rejection.** Fast (`extract_flat`), specific, honest — a correct implementation
   of "prepare, don't fully implement".
9. **Test suites pass and are not vacuous.** 146 + 24 real assertions on real logic. The
   gaps are in what is *not* covered, not in what is.
10. **Honest scaffolding.** `createJob` returns "not implemented yet" rather than pretending.
    `IImageEngine`/`IDocumentConverter` are interfaces with no fake implementations.

---

## 15. Findings — full index

39 issues filed. Two pre-existing owner-reported issues (#1, #2) were **not** duplicated;
#7 identifies the root cause of #1, and #16 expands #2.

| ID | Title | Severity | Type | Release impact |
|---|---|---|---|---|
| [#3](https://github.com/h-kakar11/gravity/issues/3) | Failed/cancelled download deletes pre-existing user files and folders | Critical | Bug / Security | **P0 blocker** |
| [#4](https://github.com/h-kakar11/gravity/issues/4) | Cancel/start race crashes the core process | High | Bug / Architecture | **P0 blocker** |
| [#5](https://github.com/h-kakar11/gravity/issues/5) | Unvalidated settings brick the app permanently | High | Security / Bug | **P0 blocker** |
| [#7](https://github.com/h-kakar11/gravity/issues/7) | Installed build ships no working backend | High | Packaging | **P0 blocker** |
| [#30](https://github.com/h-kakar11/gravity/issues/30) | No LICENSE, no third-party attribution (FFmpeg GPL) | Medium | Release / Docs | **P0 blocker** |
| [#6](https://github.com/h-kakar11/gravity/issues/6) | Shutdown starts new jobs and blocks | High | Bug / Architecture | P1 |
| [#8](https://github.com/h-kakar11/gravity/issues/8) | Blocking, uncancellable, untimed inspect freezes the backend | High | Bug / Architecture | P1 |
| [#9](https://github.com/h-kakar11/gravity/issues/9) | Process tree not terminated; orphan ffmpeg | High | Bug | P1 |
| [#10](https://github.com/h-kakar11/gravity/issues/10) | No queue persistence or crash recovery | High | Feature / Architecture | P1 |
| [#14](https://github.com/h-kakar11/gravity/issues/14) | Product named Gravity, everything says "MediaTool" | Medium | Release / UX | **P0 for release** |
| [#15](https://github.com/h-kakar11/gravity/issues/15) | Convert/Compress do not exist; getCapabilities lies | High | Feature / Architecture | P1 / P2 |
| [#16](https://github.com/h-kakar11/gravity/issues/16) | No Queue screen, navigation shell or dark theme | High | UX / Feature | P1 |
| [#11](https://github.com/h-kakar11/gravity/issues/11) | Output directory unvalidated (traversal, UNC) | Medium | Security | P1 |
| [#13](https://github.com/h-kakar11/gravity/issues/13) | Reserved device names and MAX_PATH unhandled | Medium | Bug | P1 |
| [#23](https://github.com/h-kakar11/gravity/issues/23) | Sidecar death undetected; silent 30-second timeouts | Medium | Bug / UX | P1 |
| [#25](https://github.com/h-kakar11/gravity/issues/25) | prepare_filename() output path can be wrong after merge | Medium | Bug | P1 (needs verification) |
| [#26](https://github.com/h-kakar11/gravity/issues/26) | No CI/CD; suite cannot run outside Windows | Medium | Testing / Release | P1 |
| [#27](https://github.com/h-kakar11/gravity/issues/27) | No tests for IPC loop, Rust bridge or frontend | Medium | Testing | P1 |
| [#28](https://github.com/h-kakar11/gravity/issues/28) | CSP disabled; remote thumbnails in a privileged page | Medium | Security | P1 |
| [#12](https://github.com/h-kakar11/gravity/issues/12) | Concurrent downloads can allocate the same filename | Medium | Bug | P1 (before raising concurrency) |
| [#17](https://github.com/h-kakar11/gravity/issues/17) | No priorities, dependencies or retry policy | Medium | Feature / Architecture | P2 |
| [#18](https://github.com/h-kakar11/gravity/issues/18) | Most Settings fields are inert | Medium | Bug / UX | P2 |
| [#19](https://github.com/h-kakar11/gravity/issues/19) | Per-progress-event IPC refetch; progress can rewind | Medium | Performance / Bug | P2 |
| [#20](https://github.com/h-kakar11/gravity/issues/20) | FFmpeg discovery re-spawns `where`; override unverified | Medium | Performance / Bug | P2 |
| [#21](https://github.com/h-kakar11/gravity/issues/21) | IPC parameters unvalidated; unroutable responses | Medium | Bug / Architecture | P2 |
| [#22](https://github.com/h-kakar11/gravity/issues/22) | IPC contract drift; four events never emitted | Medium | Docs / Bug | P2 |
| [#24](https://github.com/h-kakar11/gravity/issues/24) | stderr starvable and unbounded; yt-dlp stderr discarded | Medium | Bug | P2 |
| [#31](https://github.com/h-kakar11/gravity/issues/31) | 40+ formats fetched, only counted; silent preset substitution | Medium | UX | P2 |
| [#32](https://github.com/h-kakar11/gravity/issues/32) | Accessibility: no ARIA, no live regions, contrast below AA | Medium | UX | P2 |
| [#33](https://github.com/h-kakar11/gravity/issues/33) | Tracebacks shown to users; no overflow, loading or empty states | Medium | UX | P2 |
| [#34](https://github.com/h-kakar11/gravity/issues/34) | Docs overstate completeness; two safety claims are false | Medium | Documentation | P2 |
| [#29](https://github.com/h-kakar11/gravity/issues/29) | Completed jobs never removed; unbounded growth | Low | Performance / Bug | P2 |
| [#35](https://github.com/h-kakar11/gravity/issues/35) | Three metadata extractions per download | Low | Performance | P2 |
| [#36](https://github.com/h-kakar11/gravity/issues/36) | Raw `Job*` outside the lock — latent use-after-free | Low | Tech debt / Architecture | P2 |
| [#37](https://github.com/h-kakar11/gravity/issues/37) | Console windows on Windows (no CREATE_NO_WINDOW) | Low | UX | P2 |
| [#38](https://github.com/h-kakar11/gravity/issues/38) | explorer.exe argument interpolation; misleading comment | Low | Security | P3 |
| [#39](https://github.com/h-kakar11/gravity/issues/39) | Dead and unwired code | Low | Tech debt | P3 |
| [#40](https://github.com/h-kakar11/gravity/issues/40) | Dev-dependency advisories; no update process | Low | Security | P3 |
| [#41](https://github.com/h-kakar11/gravity/issues/41) | Playlist support — decide and reconcile the docs | Info | Recommendation | P3 |

### Totals

| Metric | Count |
|---|---|
| Critical | 1 |
| High | 8 |
| Medium | 22 |
| Low | 7 |
| Info | 1 |
| **Total filed** | **39** |
| Release blockers (P0) | 5 |
| Recommendations | 1 |
| Security findings | 6 (#3, #5, #11, #28, #38, #40) |
| Windows-specific findings | 5 (#7, #9, #11, #13, #37) |
| UX findings | 8 (#8, #14, #16, #18, #23, #31, #32, #33) |
| Testing gaps | 2 (#26, #27) |
| Packaging findings | 2 (#7, #30) |
| Performance findings | 5 (#19, #20, #29, #35, plus #24) |
| Documentation findings | 3 (#22, #30, #34) |
| Reproduced with a working harness | 5 (#3, #4, #5, #6, #13) |
| Needs Windows verification | 5 |
| Needs further verification | 1 (#25) |

---

## 16. Release blockers

Five issues genuinely prevent release. Everything else is severity, not a gate.

1. **[#3](https://github.com/h-kakar11/gravity/issues/3) — data destruction.** Reproduced.
   Shipping software that silently deletes a user's files during ordinary use is not
   defensible under any timeline pressure.
2. **[#7](https://github.com/h-kakar11/gravity/issues/7) — no shippable artifact.** The
   installer contains no backend. There is nothing to release.
3. **[#30](https://github.com/h-kakar11/gravity/issues/30) — licensing.** No LICENSE, no
   attribution, and the recommended FFmpeg build is GPL. A legal gate, independent of code
   quality. It also constrains *how* #7 is implemented, so it should be settled first.
4. **[#5](https://github.com/h-kakar11/gravity/issues/5) — unrecoverable brick.**
   Reproduced. One IPC call makes the app permanently unable to start, with no diagnostic
   and no user-discoverable recovery.
5. **[#4](https://github.com/h-kakar11/gravity/issues/4) — process abort on cancel.**
   Reproduced. A user-triggerable full-backend crash with total loss of in-flight work.

[#14](https://github.com/h-kakar11/gravity/issues/14) (product name) is medium severity but
a practical P0: shipping under the wrong name is not shippable, and the bundle identifier
and AppData paths get materially more expensive to change after the first release.

---

## 17. Prioritisation

### P0 — must fix before release
#3, #4, #5, #7, #30, #14

### P1 — should fix before release
#6, #8, #9, #10, #11, #12, #13, #15 *(the `getCapabilities` half)*, #16, #23, #25, #26, #27, #28

### P2 — important post-release
#15 *(the feature)*, #17, #18, #19, #20, #21, #22, #24, #29, #31, #32, #33, #34, #35, #36, #37

### P3 — future enhancement
#38, #39, #40, #41

### Recommended order of work

1. **Stop the bleeding:** #3, then #4 and #5 (both small, both crashes).
2. **Make it shippable:** #30 (decide FFmpeg licensing) → #7 (bundle everything) → #14
   (rename) → produce and test an installer on a clean Windows machine.
3. **Make it trustworthy:** #26 (CI on Windows) and #27 (IPC + concurrency tests) — every
   fix above should land with a test that fails before it.
4. **Make it reliable:** #6, #8, #9, #23, #13, #11, #12.
5. **Decide the product scope.** Either ship a *downloader* — in which case #16 (Queue,
   Settings, dark theme) plus honest docs (#34) is the remaining set — or build the full
   product, which additionally requires #15, #10 and #17. That is a product decision, and
   it should be made explicitly rather than by drift.

---

## 18. Environment limitations

Recorded honestly. Nothing below was converted into a pass.

- **No Windows host.** The audit ran on Linux x86-64. Gravity is a Windows application.
  Every Windows-specific conclusion is from code reading, not observation. Specifically
  **NOT VERIFIED:** process-tree termination (#9), console-window behaviour (#37), MinGW
  build, MAX_PATH behaviour, reserved-name behaviour on a real filesystem, DPI scaling,
  installer behaviour, and the `--selftest` FFmpeg steps.
- **No installer could be produced,** so install / upgrade / reinstall / uninstall and
  stale-file checks are entirely unverified.
- **The application could not be launched.** No display server, no Windows, and the shell
  cannot locate its sidecar. No screenshots. Note that `docs/phase-2.md` records the same
  gap for the previous session — **the running UI has never been visually verified.**
- **The C++ core does not link on Linux** (`WindowsHardwareDetector`). A one-file stub was
  added *in a scratch copy outside the repository* to build and run the suite. The audited
  repository was not modified for verification.
- **Third-party tools absent:** no `ffmpeg`/`ffprobe`, no `yt_dlp`. So real downloads, real
  merges, real probing and #25 could not be exercised.
- **Dependencies substituted:** `nlohmann-json`, `spdlog` and `gtest` from apt; `reproc`
  built from source at v14.2.5 — rather than the project's vcpkg pins (which have no
  baseline, so "the" pinned versions do not exist as such).
- **The Rust build targeted Linux/GNU**, not the MinGW Windows target the project ships.
- **No assistive technology** was run for the accessibility assessment.
- **Not attempted:** long-duration memory-growth measurement, 100-job real-download load,
  and malicious-media-file fuzzing of FFmpeg's parsers.

All reproduction harnesses were built and run outside the repository, under the session
scratch directory, and no scratch artifacts were left behind. The working tree is clean
apart from this report.

---

## 19. Exact test results

```
C++ GoogleTest (Linux, patched WindowsHardwareDetector stub in a scratch copy)
  92% tests passed, 12 tests failed out of 146
  Total Test time (real) = 4.83 sec
  Failures (all Windows-platform assumptions, not defects):
      2 - DownloadJob.DeduplicatesFilenameWhenBaseNameAlreadyExists
      3 - DownloadJob.DownloadFailureThrowsAndCleansUpArtifacts
     74 - PathUtilsTest.JoinInsertsSeparator
     75 - PathUtilsTest.JoinWithAbsoluteComponentReplacesBase
     76 - PathUtilsTest.JoinVariadicFoldsLeftToRight
     77 - PathUtilsTest.IsAbsoluteRecognizesWindowsDriveRoot
     79 - PathUtilsTest.NormalizeCollapsesDotAndDotDotSegments
     80 - PathUtilsTest.NormalizeConvertsForwardSlashesToPreferredSeparator
     83 - PathUtilsTest.GetFilenameStripsDirectory
     84 - PathUtilsTest.GetParentDirectoryStripsFilename
     85 - RealProcessRunnerTest.EchoProducesExpectedStdoutLine
     86 - RealProcessRunnerTest.KillStopsLongRunningProcessQuickly

Python unittest
  Ran 24 tests in 0.271s
  OK

TypeScript
  npx tsc --noEmit -> clean (exit 0)

Vite build
  vite v5.4.21 building for production...
  43 modules transformed.
  dist/index.html                  0.33 kB | gzip:  0.24 kB
  dist/assets/index-D1SCWXWf.js  175.68 kB | gzip: 54.10 kB
  built in 1.04s

Rust (x86_64-unknown-linux-gnu)
  cargo build   -> Finished `dev` profile in 1m 28s
  cargo clippy  -> clean
  cargo test    -> 0 passed; 0 failed  (no tests exist)

npm audit (app/frontend)
  {"info":0,"low":0,"moderate":1,"high":1,"critical":0,"total":2}
  vite   <=6.4.1   GHSA-4w7w-66w2-5vf9  (dev server only)
  esbuild <=0.24.2 GHSA-67mh-4wv8-2f99  (dev server only)

IPC fuzz (real mediatool-core, 23 adversarial NDJSON inputs)
  no memory-safety crash; no non-JSON stdout line
  correctly rejected: unknown command, bogus job type, file:// URL,
                      invalid quality enum, unknown job id, empty output dir
  poor errors:        E_UNHANDLED_EXCEPTION for missing/wrong-typed params
  wrongly accepted:   ../ traversal outputDir, UNC outputDir,
                      concurrentJobs=100000, analyticsEnabled=true
  arbitrary read:     inspectFile /etc/shadow -> ok:true

Reproduction: CleanupArtifacts data loss (#3)
  BEFORE: Song - Live at Wembley.mp4, Song Collection/{track01,track02}.flac,
          Song (1).mp4, Unrelated.mp4
  job failed as expected: E_NETWORK
  AFTER:  Unrelated.mp4
  RESULT: 4 pre-existing user file(s) destroyed

Reproduction: cancel/start race (#4)
  terminate called after throwing an instance of 'mediatool::errors::MediaToolException'
    what():  Internal error: invalid job state transition
  Aborted (exit 134)
  aborted at ~1200 / ~3400 / ~6800 submit-cancel cycles across three runs

Reproduction: shutdown drain (#6)
  before shutdown: started=1 finished=0
  shutdown returned after 2401 ms
  jobs started=6 finished=6, of which STARTED AFTER shutdown began=5

Reproduction: settings brick (#5)
  updateSettings {processing:{concurrentJobs:100000}} -> ok:true, persisted
  next launch:
    terminate called after throwing an instance of 'std::system_error'
      what():  Resource temporarily unavailable
    Aborted (exit 134)

Reproduction: reserved names / path length (#13)
  CON -> CON, NUL -> NUL, COM1 -> COM1, LPT1 -> LPT1  (all unchanged)
  300 CJK codepoints -> 200 kept (600 bytes); 200 astral codepoints = 400 UTF-16 units > MAX_PATH
```

---

## 20. GitHub issue index

See [section 15](#15-findings--full-index) for the full table with severities, types and
release impact. All 39 issues carry the `audit` label; filter with:

- `label:release-blocker` — the 5 P0 gates plus #14
- `label:severity:critical` / `severity:high` — the 9 most serious
- `label:needs-windows` — the 5 requiring a Windows host to confirm or fix
- `label:needs-verification` — #25, the one finding not confirmed
- `label:type:security` — the 6 security findings
- `label:recommendation` — #41, filed as a product decision rather than a defect

Pre-existing issues, deliberately not duplicated: **#1** (engine error — root cause
analysed in #7) and **#2** (UI improvement — expanded into #16, #31, #32, #33).

---

## Final assessment

**NOT RELEASE READY.**

Gravity has a sound foundation and a working download vertical slice, built with better
architectural discipline than most projects at this stage — clean seams, no shell execution,
honest scaffolding, and a decisions log that records real reasoning including its own
mistakes. That work is worth building on.

It is not shippable today for four independent reasons: **the installer contains no
backend**, **there is no licence and FFmpeg's licensing is unresolved**, **a failed download
destroys user files**, and **two user-reachable paths crash or permanently brick the
application**. Each of those is individually disqualifying and all four were confirmed
rather than inferred.

Beyond shippability, the product described in the brief — Convert & Compress, a unified
Queue with priorities, dependencies and persistence, a polished dark UI — is roughly a third
built. That is not a criticism of the code; the repository's own documentation says as much.
It is a correction to the framing: this is a solid Phase 2 of ten, not a release candidate.

The nearest honest release is a **downloader**, after the five blockers, the reliability set
(#6, #8, #9, #23, #13), a real Queue and Settings UI (#16), CI (#26), and documentation that
says plainly what Gravity does and does not do yet (#34). That is achievable. Calling the
current state release-ready is not supportable by anything this audit found.
