# Findings from PR #43 (`claude/gravity-phase-5-pipeline-76kjkd`)

PR #43 was an independent, parallel implementation of overlapping scope — 26,497
additions across 154 files, 43 commits, never merged. It is not being merged: a direct
code comparison against the original 39-issue audit found it still ships 6 of the 8
confirmed critical/high defects that this project's Phase 1 exists to fix, most
seriously the exact same unrestricted-recursive-delete data-loss bug (audit #3,
`DownloadJob::CleanupArtifacts`'s `name.rfind(filenameBase,0)==0` prefix match). It also
has no CI workflow at all (zero automated verification, ever) and does not implement any
of Phase 4's must-have OS-integration features (Watch Folders, tray, Scheduled Tasks,
hotkeys, presets, hardware-acceleration UI, Windows context menu) — its own roadmap
explicitly declares v1 "finished" without them.

That said, real engineering work went into it, and some of it is worth keeping even
though the branch itself isn't. This document records what to salvage: two concrete bugs
its hardening pass found that also apply to what's on `master` today, and three
architectural ideas its queue implementation has that `master`'s doesn't.

## Live bugs confirmed to also affect `master` (verified directly, not just carried over from PR #43's claims)

**Both fixed** — see the commit on `fix/live-bugs-utf8-and-cancel-hang` fixing them, with
regression tests in `JobHistoryStoreTest`, `JsonFileSettingsStoreTest`, `PresetStoreTest`,
and `ProcessRunnerTest`. Left the analysis below as-is since it's the record of how each
was found.

### 1. IPC stdout serialization crashes on invalid UTF-8 (HIGH)

`app/core/main.cpp`'s `WriteLine()` is the single chokepoint every IPC response and
event is written through:

```cpp
void WriteLine(const json& payload) {
    std::lock_guard<std::mutex> lock(g_stdoutMutex);
    std::cout << payload.dump() << std::endl;
}
```

`nlohmann::json::dump()`'s default UTF-8 handling is strict and **throws** on invalid
input. `payload` carries externally-influenced text on every job-related response/event —
video titles, ffmpeg/yt-dlp error text, file paths — none of which this process controls
the byte-level encoding of. A single invalid UTF-8 byte anywhere in that text is an
uncaught `nlohmann::json::type_error` that terminates the whole core process. Confirmed
present in `main.cpp`; the same unguarded `.dump()` pattern also appears in
`core/jobs/JobHistoryStore.cpp`, `core/settings/JsonFileSettingsStore.cpp`, and
`core/settings/PresetStore.cpp` (all persist externally-influenced text to disk).

**Fix**: pass `nlohmann::json::error_handler_t::replace` at every one of these `dump()`
call sites (e.g. `payload.dump(-1, ' ', false, json::error_handler_t::replace)`),
substituting the offending byte instead of throwing. No schema or behavior change for
well-formed input.

### 2. Cancelling a momentarily-quiet child process can hang indefinitely (HIGH)

`core/process/RealProcessRunner.cpp`'s drain loop:

```cpp
auto [events, pollEc] = process_.poll(reproc::event::out | reproc::event::err, reproc::milliseconds(200));
if (pollEc == std::errc::timed_out) {
    continue;  // no output ready -- loop back around to re-check pendingAction_
}
...
const reproc::stream which = (events & reproc::event::out) ? reproc::stream::out : reproc::stream::err;
auto [bytesRead, readEc] = process_.read(which, buffer, kBufferSize);
```

Traced directly against the vendored reproc source (`reproc/src/reproc.c`,
`reproc++/src/reproc.cpp`): when the plain 200ms poll timeout elapses with nothing ready,
reproc returns success (`r == 0`) with all `events` bits clear — `error_code_from(r)`
returns `{}` (an empty/success `std::error_code`) for any `r >= 0`, so `pollEc` is never
`std::errc::timed_out` in this case. That `continue` branch is dead code for the exact
situation its comment describes. Execution instead falls through to the `read()` call
with `events == 0`, which resolves `which` to `reproc::stream::err` and calls a genuinely
blocking `read()` on it (`reproc/src/pipe.posix.c`'s `pipe_read` is a plain blocking
`read(2)`, not toggled non-blocking here).

`pendingAction_` (set by `Kill()`/`Terminate()`) is only checked at the top of this same
loop — so a child that is alive but simply hasn't written anything in the last 200ms (the
normal state of ffmpeg between progress lines, or yt-dlp between network chunks) leaves
the drain thread blocked in that `read()`, and `Kill()`/`Terminate()` will not actually
run until the child produces output or exits on its own. This directly undermines Phase
1.5's Windows Job Object fix: the process tree is correctly killed once `kill()` actually
gets called, but cancellation can silently do nothing for an unbounded time before that
happens.

**Fix**: check `events == 0` directly (as the success case it is) rather than a
`std::errc::timed_out` comparison that reproc never actually produces here, and `continue`
on it — mirroring the fix PR #43 applied to its own equivalent code.

### Lower-severity items also noted, not yet independently verified as live bugs on `master`

- No cap on accepted download URL length before it's persisted into a duplicate-keyed job
  record (every other free-text field in the schema has one).
- No startup-time sweep for a stray output file left by a hard process crash (SIGKILL,
  power loss) mid-job — distinct from Phase 1.1's cleanup, which only covers a job that
  fails while the process is still running to catch it.

## Architectural ideas worth adopting later (not present on `master`, genuinely more advanced there)

1. **Retry classification + bounded exponential backoff.** A `RetryWait`/`Retrying` job
   state pair with a dedicated policy component deciding which failures are worth retrying
   and how long to wait, rather than a job simply ending in `Failed`.
2. **Composable job pipelines.** Jobs can name another job as their input
   (`inputFromJobId`), so `Download → Convert → Compress` runs as one dependency chain the
   scheduler resolves, instead of three manually-sequenced user actions.
3. **Durable, versioned, crash-recoverable queue state.** The full queue (not just
   settings/history/presets) persists to disk with a schema version and recovers cleanly
   after a hard restart, including jobs that were mid-flight.

None of these are required for the current product scope, but they're a reasonable v2
direction if richer queue semantics become a priority.

---
_PR #43 remains closed, unmerged, for the reasons above. This document is the record of
what was worth keeping from it._
