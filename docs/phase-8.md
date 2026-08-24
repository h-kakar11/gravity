# Phase 8 — Hardening

Phase 7 made Gravity distributable. Phase 8 tried to break it: real adversarial input
against the real binary, a real audit of the process-spawning and JSON-serialization
boundaries, and a real (if partial) investigation of an intermittent slowdown observed
along the way, rather than dismissing it because it was hard to pin down.

Two genuine, previously-undetected bugs were found and fixed — one a two-line crash
reproduction, the other a silent functional defect that has existed since Phase 1. Both are
documented in detail below because *how* they were found matters as much as the fix: neither
was visible to code review, and neither was caught by the existing test suite, because the
existing suite exercised the code paths that had the bugs almost exclusively through mocks.

## 1. System audit — trust boundaries

Traced every point where Gravity accepts input it did not itself produce:

| Boundary | What crosses it |
|---|---|
| IPC (stdin) | Frontend commands — the only channel a hostile or merely careless caller could reach without local file access |
| Child process stdout/stderr | ffmpeg, ffprobe, the Python downloader — text this process does not control the encoding or content of |
| The persisted queue/settings files | Read back on every restart; could have been hand-edited or corrupted between runs |
| The filesystem | Paths given via IPC (inputPath, outputDirectory), and whatever's actually on disk when a job runs |

Everything below is organized by which of these it actually attacked.

## 2-3. IPC fuzzing and path/injection review

`tests/e2e/ipc_fuzz.py` (new, 77 checks) sends malformed input directly at the real
`mediatool-core` binary's NDJSON stdin: missing/wrong-typed/null fields, huge strings,
invalid enums, negative and absurd numeric values, malformed and truncated JSON, unknown
commands, hostile-looking command names, unknown/malformed job ids, and — the one that
actually broke something — raw invalid UTF-8 bytes. After every case it sends an ordinary
`listJobs` and checks the response is correctly `id`'d and that nothing malformed has ever
appeared on stdout, proving the protocol stream itself stayed intact, not just that the
process didn't crash.

**Found: a two-line crash.** A request containing an invalid UTF-8 byte makes
`nlohmann::json::parse()` throw a diagnostic that embeds a snippet of the offending input;
that diagnostic became the response's `message` field; and `nlohmann::json::dump()`'s
**default** UTF-8 handling is strict and throws on invalid input — uncaught, terminating the
process (`terminate called after throwing an instance of ... type_error.316`). Any malformed
byte sent over stdin, not even maliciously, was a full crash. Fixed at the actual boundary —
every `dump()` call that can serialize externally-influenced text (IPC responses/events, the
persisted queue and settings files, the duplicate-job-key computation) now passes
`error_handler_t::replace`, substituting the byte rather than ever throwing over it.
`JobManager`'s queue-persistence catch was also broadened from `MediaToolException` to
`std::exception` — that save runs on the scheduler thread with no IPC-loop `try/catch` above
it, so the identical bug there (reachable by nothing more adversarial than a video whose
title happened to be oddly encoded) would have been an unconditional crash during ordinary
operation, not fuzzing.

**Found: no length bound on `url`.** Every other free-text field already had one
(`RequirePath` caps paths at 4096 chars); a 1MB URL was accepted whole into a persisted,
duplicate-keyed job. Capped at 8192 characters.

**Path/injection review:** confirmed (by re-reading, and by fuzzing hostile-looking paths —
`../../../../etc/passwd`, embedded NUL bytes, 5000-character paths, Windows device paths)
that every process launch in the codebase already goes through `IProcessRunner::Start` with
a structured argv vector, never a shell string — `core/process/IProcessRunner.h`'s own
header comment states this as the rule, and grepping the whole repo for `std::system`,
`popen`, `exec*p`, `ShellExecute`, `WinExec`, and Python `shell=True` / Rust
`Command::new("sh"/"cmd")` found zero matches outside that one comment. No new injection
vector was found; the existing `RequirePath` `..`-rejection and NUL-byte rejection
(spec section 54, built in Phase 5) held under fuzzing.

## 4. Process safety

`tests/core/ProcessRunnerTest.cpp`'s two tests exercising a real OS process are both
`SKIP_UNLESS_WINDOWS()` — meaning this project's actual dev/CI platform (Linux) had **zero**
test coverage of real process spawning. Every other test touching process-dependent logic
uses `MockProcessRunner`, which is correct by construction and structurally cannot catch a
bug in the real spawn path. Added `tests/core/RealProcessRunnerPortableTest.cpp` (7 tests,
portable command choice per platform) to close that gap, and it found two real bugs on the
first run:

**Found: stderr was never actually captured.** reproc's own default redirect resolves
stdout to a pipe but **stderr to `PARENT`** — inherited straight through to
`mediatool-core`'s own stderr — unless told otherwise
(`reproc/src/options.c`: `stream == REPROC_STREAM_ERR ? PARENT : PIPE`).
`RealProcessRunner::Start()` never set it explicitly. Verified the real-world consequence
against **real ffmpeg**, not a mock: fed it a corrupt input file and checked the resulting
`ErrorInfo.details` field —

```
"details": "exit code 183\n[mov,mp4,m4a,3gp,3g2,mj2 @ ...] moov atom not found\n
[in#0 @ ...] Error opening input: Invalid data found when processing input\n..."
```

— which was an **empty string in every prior build**, because
`engines/ffmpeg/FFmpegEngine.cpp`'s captured "ffmpeg's stderr at `-loglevel error`"
diagnostic had nothing to capture: the real text was silently leaking to
`mediatool-core`'s own stderr instead. Every FFmpeg/ffprobe failure's diagnostic detail has
been empty since Phase 1. Fixed by pinning both `redirect.out` and `redirect.err` to `pipe`
explicitly, rather than depending on which of two different per-stream defaults a library
happens to pick.

**Found: cancelling a silent child could hang for its entire natural lifetime.** The drain
loop's "did `poll()` time out" check compared `pollEc` to `std::errc::timed_out` — but
reproc signals "nothing became ready" by returning **success** with `events == 0`, not a
distinct error code, so that branch was dead code (confirmed with a standalone
instrumented reproc program: `pollEc.message() == "Success"`, `events == 0`, every 200ms,
exactly as designed — the code just checked the wrong signal for it). On `events == 0` the
loop fell through to `read()` on whichever stream `(events & event::out)` happened to
evaluate to — always `err` when `events` is 0 — and that call **blocks** when there's
nothing to read, for as long as the child runs. `Kill()`/`Terminate()` only take effect at
the top of this same loop, so the drain thread being stuck inside that read meant they were
never rechecked. The one prior real-process test (`ping.exe`, chattering once a second)
never exposed this — real activity kept `poll()` returning non-zero events on its own, so
this path was never exercised even on Windows. Reproduced directly: `sleep 30`, killed after
200ms, took the full 30 seconds to actually stop before the fix; 203ms after. Fixed by
checking `events == 0` directly instead of the never-true error-code comparison.

Both fixes were verified against the full E2E suite (real ffmpeg, real subprocess
cancellation) afterward, not just the new unit tests.

Also tested and passing: launching a nonexistent executable (clean `MediaToolException`, no
crash, no leaked process — confirmed no child is ever forked in that path), a child that
exits immediately with 0 and with a nonzero code, `Terminate()` correctly falling back to
`Kill()` when a child ignores SIGTERM, an enormous stderr stream (200,000 lines) proving the
concurrent stdout+stderr drain genuinely doesn't deadlock (the actual reason reproc was
chosen — see `docs/decisions.md` — verified rather than only assumed), and invalid UTF-8 on
stdout surviving untouched rather than crashing the line-splitter.

## 5. Job state fuzzing

Already extensively covered by Phase 5's `SchedulerCoreTest.cpp` and `JobManagerQueueTest.cpp`
at the unit level (dependency cycles, self/unknown/duplicate dependencies, duplicate job
ids, move-at-boundaries, running/finished jobs rejecting reorder, cancel-during-retry-wait,
retry-budget bounds, a `CancelAndCompletionRaceProducesExactlyOneTerminalState` test that is
itself a concurrency-race check) and by `JobStateMachineTest.cpp`'s exhaustive 121-combination
table. The new IPC fuzzer (§2) covers what wasn't previously exercised at this level: sending
`cancelJob`/`pauseJob`/`resumeJob`/`retryJob`/`removeJob`/`setJobPriority`/`moveJob` for an
id that was never created, and an invalid `moveJob` direction — all rejected cleanly over the
real wire protocol.

## 6. Concurrency stress

`tests/queue/SchedulerCoreStressTest.cpp` (new): 5000 seeded-random operations
(insert/dispatch/transition/retry/move/priority/cancel/dependency/history-clear) against
one scheduler, checking `ValidateInvariants()` throughout — deterministic and fast (12ms
for the whole sequence; SchedulerCore is pure, no sleeps, no threads), so a real failure
would reproduce identically from the fixed seed every time. Plus a 300-way dependency
fan-out and a 200-deep dependency chain.

**A lesson worth recording in case anyone extends this test**: an earlier version injected
raw state transitions directly (e.g. `SetState(id, Running, now)` for an arbitrary
previously-inserted job) rather than only ever transitioning a job `SelectDispatchable` had
actually admitted, and appeared to find both the concurrency limit and the retry budget
being violated. Investigation showed this was the *test* breaking a contract
`ValidateInvariants()` correctly enforces on its *caller* — `SchedulerCore` trusts
`JobManager` to only call `SetState(Running)` for a job it was told to admit, and to only
call `ScheduleRetry` for a job actually under its own retry budget; it does not re-derive
either constraint internally. Rewritten to model how `JobManager` actually drives the API
(a job enters the tracked "dispatched" set only via `SelectDispatchable`'s return value; a
retry is only requested after checking a fresh `Snapshot()` shows the job under budget), at
which point all 5000 operations produced zero invariant violations. Not a product bug —
recorded here because the instinct to treat every red assertion as a product bug is exactly
what this phase is supposed to encourage, and this is the one case in the whole phase where
that instinct was wrong.

Real concurrent job execution (not scheduler-internal simulation) is also exercised by the
E2E suites' repeated same-title race (§9) and the retry-under-concurrency scenarios (§10),
both against the real binary launching real subprocesses.

## 7-8. Filesystem and persistence failure

Already thoroughly covered by Phase 5's `QueuePersistenceTest.cpp`: empty file, whitespace-only
file, malformed JSON (quarantined, not destroyed), a non-object JSON document, missing schema
version, a future schema version (refused without overwriting), one unreadable entry not
costing the whole queue, unknown enum values falling back instead of throwing, absurd
concurrency values from a hand-edited file being clamped, and a damaged file not blocking
later saves. Re-verified this phase: all still pass, and the real E2E suite's "Corrupt state
file recovery" section (§14 of `queue_ffmpeg_e2e.py`) exercises the same property end-to-end
against the real binary — deliberately truncate `queue.json`, restart, confirm the app starts
with an empty queue, the corrupt file is preserved as `queue.json.corrupt-<timestamp>` for
diagnosis rather than silently destroyed, and the queue is fully usable afterward.

Disk-full and permission-denied write failures are handled in code
(`E_QUEUE_STATE_WRITE_FAILED`/`DiskSpaceError`, `E_SETTINGS_WRITE_FAILED`/`PermissionError`
in `QueuePersistence.cpp`/`JsonFileSettingsStore.cpp`) and degrade to "keep running
in-memory, don't crash" per `JobManager::PersistIfDue`'s catch (broadened this phase, §2) —
but a literal full-disk simulation was not performed: doing so safely in a shared sandbox
isn't practical, and is noted as a known limitation.

## 9. Output integrity — the same-title race, repeated extensively

The concurrent same-title download race (three simultaneous downloads of the same title,
which used to all report success while writing to one file — see `docs/decisions.md`,
`docs/phase-5.md`) is part of every run of `tests/e2e/queue_download_e2e.py`. Run repeatedly
this phase (12+ full-suite executions across this investigation, including the baseline
comparison in §"Investigated and ruled out" below) with zero collisions: three distinct
files every time. Cancelled-job temp-file cleanup, ffprobe verification of every completed
file, and collision-safe naming under concurrency are all exercised on every run and were
not found to regress.

## 10. Retry stress

Already covered extensively by Phase 5 (`RetryPolicyTest.cpp`, `SchedulerCoreTest.cpp`'s
`RetryBudgetIsBounded`/backoff tests) and by the E2E suite's repeated-transient-failure,
budget-exhaustion, and manual-retry-after-permanent-failure sections, all re-verified
passing this phase. No infinite-retry path exists: `ValidateInvariants()` itself now (as of
this phase's stress testing, §6) actively asserts `attempt <= maxRetries + 1` as an
invariant, not just a documented policy.

## 11. Dependency stress

`SchedulerCoreStressTest.cpp`'s 300-way fan-out (one root releasing 300 dependents in the
same dispatch pass, each exactly once) and 200-deep chain (strict in-order execution, one
link dispatchable at a time) push the dependency model — already unit-tested for
correctness in isolation by Phase 5 — to real volume. Cycles, self-dependencies, and
missing-parent dependencies are rejected at the `Insert()` boundary (Phase 5) and were
additionally re-verified at the IPC layer this phase (§2: `dependsOn` referencing an unknown
job, `dependsOn` with 1000 entries hitting the 32-dependency cap, `dependsOn` given as a
non-array).

## 12. Memory / job-history growth

`SchedulerCoreStressTest.cpp`'s chain test directly verifies the bounded-history property:
200 real job completions against a configured `historyLimit` of 64 leaves the scheduler
tracking at most 64 records afterward, not 200 — eviction under real volume, not just the
small hand-written case Phase 5's `HistoryIsBoundedAndEvictsSuccessesBeforeFailures` already
covered. A full 1000-real-job generate-and-restart timing measurement (spec's suggested
scale) was attempted this phase and abandoned as impractical within this session's time
budget (driving 1000 real `TEST` jobs through their full 10-step, 100ms/step lifecycle takes
several minutes even at high concurrency) — noted as a known limitation rather than claimed
as done. Frontend job-list rendering performance was addressed architecturally in Phase 6
(`React.memo` on `JobRow`, selective `nowMs` prop threading) and not re-measured this phase.

## 13-14. Event storm / logging stress

Progress-event throttling and coalescing (Phase 5) and the rotating log sink
(5MB × 3 files, `%LOCALAPPDATA%\Gravity\logs\`, stderr not stdout — Phase 2/7) were
re-confirmed by code reading, not re-benchmarked under synthetic load this phase.

## 15. Security review

Covered by §2-3 above (IPC fuzzing, injection/shell-command grep) plus: no hardcoded
secrets or credentials anywhere in the codebase (the application has none — no accounts, no
telemetry, by design since Phase 1); `--selftest` is intentional, documented, human-invoked
diagnostics, never reachable through the normal IPC loop, not a hidden endpoint; no
temporary file is ever created with a predictable/guessable name in a shared/world-writable
location (`TempDirectory`/`AtomicWriter` use per-job-id subdirectories under
`%LOCALAPPDATA%\Gravity\`, a per-user directory).

## 16. Performance

Measured directly against the real binary:

| Measurement | Result |
|---|---|
| Cold start to first IPC-ready event | 6.9ms |
| Single IPC round trip (empty queue) | 0.3ms |
| 100 sequential IPC round trips | 6.8ms total (0.07ms average) |
| 5000 scheduler operations (insert/dispatch/transition/retry/move/priority/cancel) | 12ms |
| 200-job dependency chain, full execution | 32ms |

All comfortably fast; nothing here suggested a bottleneck worth chasing further, per spec's
own "optimize only measured bottlenecks" instruction. A 1000-real-persisted-job restart-time
measurement was attempted and not completed within this session (§12) — the closest
available evidence is the 200-job stress test's 32ms, which shows no sign of concerning
scaling behavior at that order of magnitude.

## Investigated and ruled out: intermittent E2E slowness in this sandbox

While repeating the same-title race (§9) for extra confidence, the E2E suite occasionally
(roughly 1 run in 7-8, across ~25 total runs during this investigation) took far longer than
its typical ~15-16 seconds — twice exceeding a 100-second bound entirely. Per this phase's
own instruction not to dismiss a reproducible problem merely because it's hard to reproduce,
this was investigated rather than shrugged off:

- **Not a leak or deadlock**: checked the process tree mid-hang — the real `mediatool-core`
  and its child were present and, when given enough time, the run completed successfully
  (`PASSED 34/34`) rather than hanging forever. No process was ever left running after any
  attempt.
- **Not tied to one code path**: occurrences were caught stalled at different points across
  different runs (once after the same-title race section, once inside the manual-retry
  section, once inside the dependency-pipeline section) — inconsistent with a single race
  condition in one specific piece of logic, which this fully deterministic (no real
  randomness in the fake downloader) test harness would otherwise be expected to hit at a
  repeatable point.
- **Not caused by this phase's changes**: built the pre-Phase-8 commit (`314d06c`) in an
  isolated worktree and ran the identical repeated-scan against that binary. The same
  intermittent slowness reproduced at approximately the same rate, including a stall at the
  exact same point (`"inspect reaches the real provider"`) observed on the post-fix binary —
  direct evidence this is not a regression introduced by the crash/stderr/kill-hang fixes
  above.
- **Not correlated with system load**: `uptime`'s load average read effectively 0 both
  during a normal run and immediately after an observed slow one, with no leaked processes
  and 13GB+ free memory throughout.

Conclusion: most consistent with CPU-scheduling variance in this specific shared,
sandboxed session (a cgroup quota pause or hypervisor "stolen time" interval invisible to
in-container load metrics) rather than a defect in Gravity itself. Recorded here as an
**accepted, investigated limitation of this development environment**, not swept under the
rug — a real Windows target-machine test pass (already a known gap per `docs/phase-7.md`)
would be the way to confirm this doesn't reproduce outside this sandbox.

## Regression (full suite, after Phase 8)

| Suite | Result |
|---|---|
| C++ (`ctest`) | 362 tests (10 new this phase), 347 pass, 15 skipped (Windows-only), 0 fail |
| Python (`unittest`) | 24 pass, unchanged |
| Frontend (`vitest`) | 75 pass, unchanged (no frontend code touched this phase) |
| E2E: real ffmpeg + real binary | 75/75, re-run repeatedly |
| E2E: real downloads, retries, dependencies | 34/34, re-run 12+ times across the investigation above |
| E2E: IPC fuzzing (new) | 77/77, stable across repeated runs |

## Known limitations

1. A literal disk-full write failure was not simulated for real (§7-8) — the code path and
   its structured error/graceful-degradation behavior were verified by reading and by the
   broadened exception catch, not by an actual full-disk reproduction.
2. A genuine 1000-real-job restart-timing measurement was not completed (§12, §16) — judged
   not worth the multi-minute cost of generating it for the marginal evidence beyond the
   200-job stress result already gathered, but not claimed as done either.
3. The intermittent sandbox slowness above was investigated and its most likely cause
   identified, but not eliminated or reproduced outside this session's specific environment
   — it should be watched for, not assumed fixed, on a real target machine.
4. Frontend performance under real load (dozens of concurrently-updating job rows) was not
   re-measured this phase; Phase 6's architectural mitigations (`React.memo`, selective prop
   threading) were not re-benchmarked.

Phase 9 was not started.
