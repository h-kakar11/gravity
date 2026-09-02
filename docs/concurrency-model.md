# Concurrency model

What runs on which thread, what is allowed to happen at the same time as what, and the
rules that make those two answers safe to rely on. Read `docs/architecture.md` first for
the process layout and `docs/ipc-contract.md` for the wire protocol; this document is
about the C++ core's internals, which is where all of the concurrency is.

Everything here is a description of the code as it stands, not an aspiration. Where a
guarantee is weaker than it sounds, that is said explicitly.

## The threads that exist

| Thread | Count | Created by | Runs |
|---|---|---|---|
| Request loop | 1 | `main()` | Reads NDJSON from stdin, validates, routes, writes responses for non-blocking commands |
| Request executor | 4 (bounded) | `ipc::RequestExecutor` | Blocking commands only: `inspectDownloadUrl`, `inspectFile` |
| Job workers | `processing.concurrentJobs` (1–25, settings-bounded) | `jobs::JobManager` | `Job::Execute()`, one job at a time per thread |
| Subprocess readers | 2 per running subprocess | `process::RealProcessRunner` | Draining a child's stdout/stderr |

There is no thread pool anywhere else. In particular there is no timer thread: the
inspect deadline is a `steady_clock` comparison inside the cancellation callback the
subprocess poll loop already calls every 200 ms.

## Locks, and the order they may be taken

| Lock | Guards |
|---|---|
| `Job::mutex_` | One job's state, progress, error, result, metadata, timestamps, dependency list |
| `JobManager::mutex_` | The job map, the `SchedulerCore` instance, the subscriber callbacks, `stopping_` |
| `RequestExecutor::mutex_` | The task queue |
| `g_stdoutMutex` (main.cpp) | One NDJSON line at a time on stdout |
| Store mutexes (`JobHistoryStore`, `JsonFileSettingsStore`, `PresetStore`) | One file's read-modify-write |
| `FFmpegEngine`'s three `std::once_flag`s | Lazy initialization of the ffmpeg/ffprobe path and encoder-list caches |

**The ordering rule is that there isn't one, because no two of these are ever held at the
same time.** That is not an accident of the current code; it is the invariant that keeps
this deadlock-free without anyone having to remember a hierarchy:

- A `Job` fires its state-changed and progress callbacks with `Job::mutex_` released.
  `JobManager` is the subscriber, and it takes its own lock inside those callbacks.
- `JobManager` never calls a `Job` mutating method (`RequestCancel`, `Mark*`) while
  holding `mutex_`. It copies what it needs out from under the lock, releases, then calls.
- `SchedulerCore` takes no locks at all -- it is called only under `JobManager::mutex_`.
- The stores are leaves: they call nothing that calls back.
- `FFmpegEngine`'s caches use `std::once_flag` rather than a mutex, because the hazard
  there is not mutation but *initialization*: `AvailableEncoders()` returns a reference
  into a cache, so two threads filling it at once would hand one of them a reference to a
  set the other is still assigning. Exactly-once initialization makes the returned
  reference valid for the engine's lifetime.

If you add a call that violates this, the deadlock will not be in your test run; it will
be in a user's queue at concurrency 4.

## Job state transitions

`core/jobs/JobStateMachine.h` owns the transition table. `Job` owns one job's state.
`JobManager` drives it.

```
QUEUED ──► STARTING ──► RUNNING ──► COMPLETED
   │           │           │  ▲          
   │           │           │  └── PAUSED ──► (RUNNING)
   │           │           │
   │           └───────────┴──► FAILED ──► RETRYING ──► RUNNING
   │                                │
   └──────────────────────────────► CANCELLED
```

### Transitions report, they do not throw

Every transition entry point returns a `TransitionResult`:

| Result | Meaning | Who cares |
|---|---|---|
| `Success` | It happened; timestamps updated, callback fired | Everyone |
| `AlreadyInState` | Idempotent no-op | Nobody; the caller's intent already holds |
| `AlreadyTerminal` | A different terminal state got there first | The caller lost a race, which is normal |
| `InvalidTransition` | Illegal from the current, non-terminal state | A bug: logged where observed |

This is the shape of the fix for the audit's #4. The old API threw
`E_INVALID_JOB_TRANSITION` from `MarkStarting()`, and the losing party in a routine race
was a worker thread, where an escaping exception is `std::terminate` and the loss of every
in-flight job. Distinguishing "you lost a race" from "you have a bug" in the *return type*
means the common case is a branch, and the pathological case is still loud.

The consequence for callers: **the transition is the check**. Do not read `State()` and
then act on what it said -- that is the check-then-act pattern the race lived in. Attempt
the transition and inspect the result. `JobManager::PauseJob`, `ResumeJob` and `RetryJob`
all do this, and turn a non-`Success` result into the user-facing "job must be Running to
be paused" error.

### Who finalizes a job

Exactly one party, decided by where the job is:

- **Still queued.** `RequestCancel()` transitions it to CANCELLED itself, because no
  worker will ever pick it up to notice a flag.
- **Running.** `RequestCancel()` only sets the flag and wakes any paused wait. The worker
  running `Execute()` finalizes it, on the convention that a cancelled job throws
  `ErrorCategory::Cancelled` rather than returning normally. If a job type returns
  normally anyway, `RunJob` checks `IsCancellationRequested()` and records CANCELLED --
  work that was cut short is never reported as success.

## Scheduling

`jobs::SchedulerCore` decides what runs next. It has no threads, no locks and no knowledge
of `Job`; `JobManager` holds it under `mutex_` and remains the only component that touches
threads. The split is what makes the policy testable: `tests/core/SchedulerCoreTest.cpp`
covers priority, dependency and cycle behavior in ~30 tests without starting a single
thread.

**Ordering.** Highest priority first; equal priorities keep submission order (FIFO). A
retried job re-enters at the back of its priority band -- it already had a turn.

**Eligibility.** A job may start when every job it `dependsOn` has COMPLETED, and every job
it `runAfter` has reached *any* terminal state. A job whose dependencies are unmet is
*skipped, not blocking*: a ready low-priority job runs ahead of a waiting high-priority one,
because idling the pool on one stalled chain is worse than running the queue out of priority
order.

**Two edge kinds, and the difference is failure semantics.** `dependsOn` couples outcomes:
the successor is meaningless if the predecessor failed, so it is cancelled (see "Failure
propagation" below). `runAfter` couples only *timing*: the predecessor merely has to be
finished, and a predecessor that failed or was cancelled releases its successor rather than
stranding it. Nothing propagates along a `runAfter` edge, which is why those edges are not
recorded in the scheduler's reverse-edge map at all.

Use `dependsOn` for a workflow (download → convert → compress). Use `runAfter` to serialize
work that is otherwise independent: a playlist chains its entries with `runAfter` so they
download strictly one at a time, and one unavailable video costs exactly one video instead
of cancelling every entry after it (issue #41).

**Concurrency cap.** The worker pool's size, and nothing else. There is no separate
"running" counter to get out of sync: N threads each running one job is N concurrent jobs.
`MaxConcurrentJobs()` reports the pool that actually started, which can be smaller than
the one requested if the OS refused a thread -- a smaller pool runs everything correctly,
just less of it at once, and is much better than a process that fails to start.

**Cycles are impossible, not detected.** A dependency must name a job that has already
been submitted, so every edge points backwards in submission order and the graph is a DAG
by construction. An attempt to close a loop fails at the point where the earlier job would
have to name the later one. A dependency that has already FAILED or been CANCELLED is
refused at submission too: a job that can never run is better rejected while the caller
still has somewhere to put the error.

**Failure propagation.** When a job ends in anything but COMPLETED, its direct pending
`dependsOn` dependents are cancelled (`runAfter` successors are released, not cancelled). Each of those cancellations reports its own terminal state,
which strands *its* dependents, and so on -- the chain unwinds one link at a time through
the normal callback path rather than in a traversal, so there is one code path for
"dependency did not complete" regardless of chain length.

## The IPC loop

One thread reads stdin, and it must keep reading. Two rules protect that:

1. **Bounded reads.** `ipc::ReadBoundedLine` refuses a line over 1 MiB, discards it up to
   the newline, and lets the loop continue. An unbounded `std::getline` on a pipe is an
   unbounded allocation driven by whoever is on the other end.
2. **Blocking commands run elsewhere.** `inspectDownloadUrl` waits on a yt-dlp subprocess
   doing network I/O: 1-3 seconds normally, up to its 30-second deadline against an
   unresponsive site. Running that on the loop means a `cancelJob` the user just clicked
   sits unread in the pipe for that whole time. Those commands go to
   `ipc::RequestExecutor` (4 threads, 64 queued) and the loop returns to reading
   immediately.

Nothing about the wire protocol changes: responses are correlated by `id`, and
`docs/ipc-contract.md` has always said requests may complete out of order. `WriteLine`
serializes stdout, so a response written from an executor thread is indistinguishable from
one written by the loop.

When the executor is saturated, a request is answered with `E_CORE_BUSY` (recoverable)
rather than being run inline or silently dropped. Backpressure is a deliberate answer;
an unbounded queue is how a busy period becomes an out-of-memory kill.

Every parameter is validated before a handler sees it (`core/ipc/RequestValidation.h`).
Type errors, missing fields and out-of-range numbers are specific, documented error codes,
not `E_UNHANDLED_EXCEPTION` carrying nlohmann's exception text.

## Filename reservation

Two jobs downloading videos with the same title into the same folder must not both decide
they own `Song.mp4`.

Probing the disk for a free name and later writing to it are two steps, and at concurrency
> 1 another job fits between them. `filesystem::FilenameReservationRegistry` closes that
window by making the check and the claim one atomic step under one process-wide lock:

```
1. [registry lock]  List the directory once; find a base name that is free on disk AND
                    not claimed by another live reservation; insert the claim.
2. [no lock]        Download to that name. The claim is held for the whole job.
3. [registry lock]  Reservation destructor releases the claim (success or exception).
```

The reservation is an RAII guard, so the name is released on every exit path from
`Execute()` -- including the ones that throw. Scope: this is a *process-wide* claim.
A second Gravity instance, or another program writing into the same folder, is not
coordinated with and is out of scope, same as it was before the registry existed.

Cleanup after a failure deletes only files `filesystem::IsJobArtifactOf` recognizes as
this job's, via `DeleteFile()` (which refuses directories), never a recursive delete and
never a bare prefix match. That is the audit's #3, and the reservation is what makes it
sound: the base name was chosen not to collide with anything that predates the job, so
anything matching it was created by this run.

## Files written from more than one thread

`JobHistoryStore::Append` is a read-modify-write of one JSON file, called from the job
state-changed callback -- which runs on worker threads. At `concurrentJobs > 1`, two jobs
finishing together used to both read the same history, both append their own entry, and
the second rename would silently discard the first. It is serialized now, and each write
goes through a temp path unique per call (`paths::UniqueTemporarySibling`) so no two
writers can ever share a scratch file. The settings and preset stores are serialized for
the same reason, now that handlers can run on executor threads.

The atomicity guarantee is the rename: a crash mid-write leaves the old file intact and a
stray temp file behind, never a half-written store.

## Shutdown

`JobManager::Shutdown()`, in order:

1. Set `stopping_`, take every pending job off the scheduler, clear the pending set.
2. Cancel those (they were never started, so they finalize immediately).
3. Request cancellation of everything RUNNING.
4. Wake and join the workers.

Cancelling *before* joining is what stops shutdown from starting fresh work and then
waiting for it (the audit's #6). Shutdown imposes no timeout of its own: a job whose
`Execute()` ignores `IsCancellationRequested()` still blocks it. That is a property of the
job type, not of the manager, and the job types in the tree all poll the flag.

`RequestExecutor::Shutdown()` discards queued-but-unstarted tasks and waits for running
ones. Queued requests are dropped deliberately: shutdown happens when stdin has closed, so
the client that sent them is gone and no response would reach anyone. A running task may
be holding a subprocess, so it is always waited for.

## Worked example: a cancel racing a start racing a filename

Two jobs for the same video title, concurrency 2, and the user cancelling one of them at
the worst possible moment.

| Time | Worker A | Worker B | IPC / executor thread |
|---|---|---|---|
| t0 | Takes job-1 from the scheduler | Takes job-2 | — |
| t1 | `MarkStarting()` → `Success` | — | User clicks cancel on job-1 |
| t2 | — | `MarkStarting()` → `Success` | `CancelJob(job-1)`: job-1 is STARTING, so this sets the flag only |
| t3 | `MarkRunning()` → `Success` | `MarkRunning()` → `Success` | Returns `{}` to the caller immediately |
| t4 | Reserves `Song` (claim taken) | Reserves: `Song` is claimed, disk has no `Song (1)` → claims `Song (1)` | — |
| t5 | Sees the flag, throws `Cancelled` | Downloads to `Song (1).mp4` | — |
| t6 | `MarkCancelled()` → `Success`; reservation released; artifacts named `Song.*` deleted | — | — |
| t7 | Callback → `RecordTerminal(job-1, CANCELLED)`; job-3 depended on job-1, so it is returned and cancelled | — | — |

What the old code did at t2 instead: if the cancel had landed one instruction earlier,
while job-1 was still QUEUED, `RequestCancel` would transition it straight to CANCELLED,
`MarkStarting()` would throw on the worker thread, and nothing would catch it. Every job in
the process, including job-2's half-written download, would be lost to `std::terminate`.

Two things to note about t4. Job B's reservation is correct *because* job A holds a claim
the disk knows nothing about -- `Song.mp4` does not exist yet at that moment, so a
disk-only check would hand both jobs the same name. And at t6 job A deletes only its own
`Song.*` artifacts; `Song (1).mp4`, which job B is still writing, does not match
`IsJobArtifactOf("Song", ...)` because the character after the base name is not a `.`.

## Cost of the scheduling operations

Measured on the Linux build host with an optimized build, `SchedulerCore` in isolation
(no threads, no jobs actually running):

| Operation | 1,000 queued | 10,000 queued |
|---|---|---|
| `Submit` (whole queue) | 2.4 ms | 27 ms |
| `Submit` (worst single call) | 0.05 ms | 0.06 ms |
| `TakeNextEligible` (worst single call) | 0.02 ms | 0.05 ms |
| `HasEligible` with *every* job blocked on one dependency | — | 16 ms |

The pending set is a `std::map` keyed by (priority, submission sequence), so submitting and
taking are logarithmic and the queue is stored in scheduling order rather than sorted on
access. The one linear case left is the degenerate one in the last row: when nothing at all
is eligible, `HasEligible` has to look at every pending entry to establish that. It is
bounded, it only happens while the pool would be idle anyway, and making it constant would
mean a second index to keep in sync with the first -- not worth the invariant.

These are the scheduler's own costs. They are not the cost of running 10,000 jobs, which is
dominated entirely by the downloads and encodes themselves.

## What is deliberately not guaranteed

- **No cross-process coordination.** Filename reservations, settings writes and history
  appends are safe within one Gravity process. Two instances are not coordinated.
- **No crash recovery.** A job in flight when the process dies is lost, and its `.part`
  files stay on disk. `JobHistoryStore` records terminal jobs only; it is not a resume
  mechanism. This is a known gap (audit #10), not an oversight of this document.
- **No fairness beyond priority and FIFO.** A long job at high priority can starve lower
  priority work at concurrency 1. That is what priority means.
- **Snapshot fields are individually consistent, not collectively atomic.** A
  `JobSnapshot` reads several `Job` getters in turn, so under concurrent mutation one
  field can be a moment newer than another. It is a status report, not a transaction.
