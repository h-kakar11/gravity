# Phase 5 — production download pipeline, queue orchestration & reliability

Phase 5 turns the job infrastructure into a single unified media-job queue: one queue that
downloads, conversions and compressions all live in, with real concurrency control,
priorities, reordering, dependencies, bounded retries, durable state, and a queue UI.

This document describes what was actually built and why. `docs/architecture.md` covers how
it fits the rest of the system, `docs/ipc-contract.md` is the wire format, and
`docs/decisions.md` records the decisions taken along the way.

---

## What this phase found before it started

The specification for this phase describes Phases 1–4 as complete and asks for orchestration
on top of them. The repository was at **Phase 2**. `FFmpegEngine::Convert` and `Compress`
threw `E_NOT_IMPLEMENTED`, `createJob` rejected `CONVERSION` and `COMPRESSION` outright, and
there was no `ConversionJob`, `CompressionJob`, `IMediaProcessor`, `FFmpegArgumentBuilder` or
equivalent anywhere in the tree. `README.md` said as much: "Scaffolded only… `createJob`
returns an honest 'not implemented yet' error for these."

A unified queue over one job type is not a unified queue. So this phase also built the
minimum real conversion and compression layer needed for the queue to genuinely span three
job types — enough to be real, not the full Phase 4 vision. See
`docs/decisions.md` → "Building the missing conversion/compression layer" for what was
included and what was deliberately left out.

---

## Architecture

### One queue, not three

There is exactly one queue. There is no download queue, conversion queue or compression
queue, and no job type has a scheduling path of its own.

`JobManager` is that queue. It is the Phase 1 `JobManager` evolved rather than a new
orchestrator layered on top: its entire Phase 1 public surface still exists and behaves the
same way, and what changed is the machinery underneath plus the queue operations added
beside it. Layering was considered and rejected — it would have produced exactly the
competing-queues problem this phase exists to avoid, with jobs pending in one structure
while another also believed it owned them.

### Decides vs. executes

```
core/queue/SchedulerCore     DECIDES    ordering, priority, concurrency admission,
                                        dependency gating, retry timing
                                        — pure, deterministic, no threads, no clock

core/jobs/JobManager         EXECUTES   owns Job objects, runs them on worker threads,
                                        keeps records in step, emits events, persists
```

`SchedulerCore` holds no threads, no locks, no `Job` objects, and never reads a clock: every
method that needs the current time takes it as a parameter. That is what makes the whole of
this phase's scheduling behaviour testable as ordinary function calls, with no sleeps and
nothing that can flake. Its 48 tests run in 2 milliseconds.

### Threading model

- **One scheduler thread.** The only thing that dispatches. Each pass: reaps finished
  workers, resolves dependency transitions, asks `SchedulerCore` what may start, launches a
  worker per dispatched job, flushes coalesced progress, persists if dirty. It sleeps on a
  condition variable until woken or until the next retry deadline.
- **One worker thread per running job**, created on dispatch, joined by the scheduler.
- **Concurrency is enforced by the scheduler's admission decision, not by a thread pool's
  size.** That is what lets the limit change at runtime, and what makes it a cap on *real
  running processes* rather than on job objects.

Two locking rules keep it deadlock-free, and neither is incidental:

1. `mutex_` is **never** held while calling into a `Job` — `Job` methods fire callbacks that
   re-enter `JobManager`.
2. `mutex_` is **never** held while joining a worker thread — that worker is very likely
   blocked trying to enter `JobManager`.

Violating rule 1 produced a real deadlock during development: `RetryJob`'s skipped-job branch
called `NotifyQueueChanged()` inside a `lock_guard` scope, and `std::mutex` is not recursive.

`Job` objects are held by `shared_ptr`, so clearing history or removing a job can never pull
the object out from under the thread executing it.

---

## Job states

```
QUEUED      ready to run as soon as a slot frees
WAITING     blocked: a dependency has not completed successfully yet
STARTING    dispatched, about to run
RUNNING     executing
PAUSED      cooperatively paused (TestJob only — see "Pause semantics")
RETRY_WAIT  an automatic retry is scheduled; waiting out its backoff
RETRYING    a retry has been picked up and is about to run
COMPLETED   finished successfully
FAILED      finished unsuccessfully
CANCELLED   stopped by the user
SKIPPED     will never run: a dependency failed or was cancelled
```

Transitions (the authoritative table lives in `core/jobs/JobStateMachine.h`, and
`JobStateMachineTest` checks all 121 combinations against it):

```
QUEUED     -> STARTING, CANCELLED, WAITING, SKIPPED
WAITING    -> QUEUED, CANCELLED, SKIPPED
STARTING   -> RUNNING, FAILED, CANCELLED
RUNNING    -> PAUSED, COMPLETED, FAILED, CANCELLED
PAUSED     -> RUNNING, CANCELLED
RETRY_WAIT -> RETRYING, CANCELLED
FAILED     -> RETRYING, RETRY_WAIT
SKIPPED    -> WAITING
RETRYING   -> RUNNING, FAILED, CANCELLED
COMPLETED  -> (none)
CANCELLED  -> (none)
```

Why each added state earns its place:

- **`WAITING`** is separate from `QUEUED` because `QUEUED` must mean "runnable the moment a
  slot frees up" — the scheduler's admission logic depends on that being true.
- **`SKIPPED`** is separate from `FAILED` because nothing about the skipped job went wrong.
  Reporting a conversion as failed when its download failed misattributes the fault and
  makes the user look in the wrong place.
- **`RETRY_WAIT`** is where cancellation during backoff is made to stick: no worker thread
  is running, so nothing would ever notice a cancellation flag, and `Job::RequestCancel`
  therefore finalizes the transition itself. `CANCELLED` has no outgoing transitions, so the
  pending attempt can never start.

**`PAUSING` was deliberately not added.** The spec suggests it, but there is no asynchronous
pause handshake in this architecture for such a state to sit in — pause is either cooperative
and immediate, or unsupported for that job type. Adding it would have meant a state nothing
could ever be observed in.

---

## Scheduling

### Ordering

One pending list, which the user can reorder directly. Selection walks it, keeps what is
eligible right now, and picks by:

1. **effective priority**, highest first (base priority + fairness aging)
2. **position in the pending list**, earliest first

So equal priority is FIFO, and "move to top" means something. Priority affects *pending*
work only: raising a running job's priority never preempts or restarts it.

`SelectDispatchable` marks what it returns *before* returning it, which is what makes two
consecutive calls unable to hand the same job to two threads.

### Concurrency

A single configurable limit, default 2, settable at runtime between 1 and 16. Lowering it
below the number of currently running jobs does not kill anything — it just stops new work
starting until the count falls below the new limit.

Verified against real process counts, not job objects: the end-to-end suite polls
`pgrep -c -P <core pid> ffmpeg` throughout and asserts the peak never exceeds the limit.

### Fairness

A steady stream of HIGH-priority work must not starve a NORMAL job forever. A pending job
gains one rank of priority for every `agingIntervalMs` (default 30s) it waits, capped at
`maxAgingBoost` (default 20). Priority tiers are spaced 10 apart, so aging can lift a job up
to two full tiers and no further — enough to escape starvation, not enough to make priority
meaningless.

Aging measures from when a job last *entered* the pending set, not from its creation, so a
job that has already had a turn and is now retrying does not come back pre-aged.

### Resource-aware scheduling

Not implemented, deliberately. Downloads are network-bound and encodes are CPU-bound, so a
future policy could sensibly run more of the former than the latter. The scheduler is
structured to allow it — admission is one central decision in `SelectDispatchable`, with the
job's type available on every record — but a real policy needs measurements this phase has
not taken, and guessing at weights would be worse than the honest single limit.

---

## Retries

### Classification

Retry classification is **allow-list shaped and defaults to not retrying**. An error is
transient only when there is a specific reason to believe a second attempt might go
differently; everything unrecognized is permanent.

That default matters. A retry that can never succeed costs the user time, burns the budget,
and buries the real error under a pile of identical ones.

Classification reads the structured `ErrorInfo` — category first, then the machine `code` —
and **never** free-text stderr, which would break the moment yt-dlp or FFmpeg reworded a
message. Code-level rules override the category in both directions:

| Category | Default | Notable overrides |
|---|---|---|
| `NETWORK_ERROR` | transient | `E_DOWNLOAD_NOT_FOUND`, `E_DOWNLOAD_UNAVAILABLE`, `E_DOWNLOAD_HTTP_4XX`, `E_INVALID_DOWNLOAD_URL` → permanent |
| `DOWNLOAD_FAILURE` | permanent | `E_DOWNLOAD_TRANSPORT_ERROR`, `E_DOWNLOAD_HTTP_5XX`, `E_DOWNLOAD_RATE_LIMITED`, `E_DOWNLOAD_INCOMPLETE` → transient |
| `ENGINE_FAILURE` | permanent | `E_FFMPEG_STALLED` → transient |
| `FILE_NOT_FOUND`, `INVALID_FILE`, `UNSUPPORTED_FORMAT`, `PERMISSION_ERROR` | permanent | — |
| `DISK_SPACE_ERROR` | permanent | needs user action, not a timer |
| `CANCELLED` | permanent | the user asked for this |
| `UNKNOWN` / anything else | permanent | — |

`DOWNLOAD_FAILURE` defaults to permanent because it is genuinely ambiguous: it covers both
"the connection dropped" and "this video is private". Without a specific code saying which,
the safe default applies.

`E_JOB_INTERRUPTED` (a job that was mid-flight when the process died) is classified
**permanent**, which is worth spelling out. It is genuinely *unknown* rather than permanent,
but the classifier has two buckets and the rule is that uncertainty means no automatic
retry. Auto-restarting these would re-download gigabytes, or re-run an encode over a partial
file, without the user asking. The error is marked recoverable so the UI offers Retry, and
the decision stays theirs.

### Backoff

`initialDelayMs * multiplier^(attempt-1)`, clamped to `maxDelayMs`. Defaults: 3 retries,
2s initial, 60s max, ×2. Every value is validated on the way in — `maxRetries` is capped at
20 so an unbounded retry loop cannot be configured, and a multiplier below 1.0 (each retry
sooner than the last) is rejected.

Computed in `double` and clamped rather than accumulated in `int64`: a large multiplier
overflows an integer accumulator long before the clamp would catch it, and an overflowed
delay is a scheduling bug that only appears on the unlucky run.

**No jitter, deliberately.** Jitter desynchronises many clients hammering one server; this
is a single local desktop app retrying its own handful of jobs, so it would buy nothing and
cost the exact determinism the retry tests depend on.

### Manual retry

Manual retry reuses the automatic path rather than adding a second one: the job goes into
`RETRY_WAIT` with its deadline set to *now*, so it is eligible on the very next dispatch.
One dispatch path instead of two is what keeps "a job never runs twice" tractable.

Job identity is preserved — a retry is the same job, not a clone. Retrying a `SKIPPED` job
sends it back through dependency evaluation rather than assuming it can run.

### Cleanup between attempts

Every retry starts from a valid filesystem state. `MediaProcessingJob` records the output
path each attempt chose and sweeps exactly that path (plus its `.processing` sibling) before
the next attempt, so a job retried three times reclaims its own name instead of accumulating
`clip (1).mp3`, `clip (2).mp3`, `clip (3).mp3`.

The sweep is keyed on **what this job produced**, never on "whatever currently sits at the
name we want". That distinction is load-bearing: an earlier version deleted the latter and
would have destroyed a user's unrelated `clip.mp3` on the very first attempt.

---

## Pause semantics

**Queue pause means: do not start additional work.** Jobs already running continue to
completion. Queued jobs stay queued, retry timers keep counting but no attempt begins, and
the queue's state is visible to the frontend throughout.

**Per-job pause is unsupported for downloads, conversions and compressions**, and the
backend says so plainly (`E_JOB_INVALID_OPERATION`, "This job type cannot be paused; cancel
it instead") rather than pretending. There is no reliable, cross-platform way to suspend a
running `yt-dlp` or `ffmpeg` process, and the alternative — killing it and restarting later —
is not pausing: it discards work and would need resumable-output support neither tool is
being asked for here.

Showing "Paused" over a process still consuming CPU and bandwidth would be a lie, so the UI
never offers the control for those job types. `TestJob` keeps its cooperative checkpoint
pause, which is genuinely real, and `PAUSED` exists for it.

---

## Dependencies

A job may declare dependencies on other jobs. It starts in `WAITING`, becomes `QUEUED` when
every dependency has `COMPLETED`, and becomes `SKIPPED` if any dependency fails, is cancelled
or is itself skipped — so skips propagate down a chain.

Rejected at submission, with nothing left behind: unknown dependency ids, self-dependencies,
duplicate entries, and cycles (checked by walking the existing graph before insertion).
More than 32 dependencies is rejected at the IPC boundary.

### The pipeline problem

`download → convert → compress` needs the second stage to consume the first stage's output.
That path is **not knowable when the pipeline is declared**: yt-dlp names the file from the
media's title and whichever container the extractor chose, and deduplication may then have
moved it to `Title (2).mp4`.

So a stage may name the job it reads from (`inputFromJobId`) instead of a path. The backend
resolves it from that job's recorded output immediately before the follower executes — by
which point the scheduler has already guaranteed the producer completed — and declaring it
implies the dependency, so the stage cannot be dispatched early or run against a file that
was never produced.

This is what stops the frontend from polling for completion and then constructing a second
unrelated operation. It also stops it from *guessing*, which an earlier version did and which
was wrong for exactly the download case the feature exists for.

---

## Duplicate detection

Two pending requests are duplicates iff their identity keys are byte-identical, where the key
is the job type plus the canonical serialization of its params. `nlohmann::json` orders object
keys deterministically, so the key is stable regardless of what order the frontend wrote them
in.

**Policy: reject, and name the job it collided with** (`E_DUPLICATE_JOB`, with the existing
job's id in `details`), so the UI can focus the existing job rather than quietly starting a
second identical download. `allowDuplicate: true` overrides it explicitly.

Anything less exact than byte-identity is treated as a different request. Merging
merely-similar jobs silently loses user intent — converting a file to MP3 and to WAV are two
requests, not one.

Only *active* jobs are checked, so the same request is allowed again once the first has
finished.

---

## Persistence and recovery

### Format

A single versioned JSON document (`schemaVersion: 1`) at
`%LOCALAPPDATA%\MediaTool\queue.json`, written through `filesystem::AtomicWriter`.

`AtomicWriter` was evaluated for this and is the right tool: it already implements exactly
the write-temp / rename-over pattern this needs, and its destructor removes the temporary on
every error path, so a failed save cannot leave debris.

What is persisted: scheduling metadata (state, priority, order, dependencies, retry counters
and deadlines, timestamps, descriptive metadata) plus the `JobSpec` needed to rebuild the
job. What is **not**: live process handles, OS PIDs, progress percentages, open file
descriptors — none of them mean anything in the next process.

Rebuilding is done by `IJobFactory`, so `createJob` and restart recovery construct a job the
same way; the validated params are what get persisted.

### Durability model

Writes are triggered by **durable changes only** — job creation, state transitions, priority
changes, reordering, retry scheduling, pause/resume, concurrency changes, completion — and
then coalesced to at most one write per `persistIntervalMs` (default 500ms), plus a forced
write on shutdown.

**Progress never marks the queue dirty.** Persisting on every progress tick is precisely the
design the spec rules out, and there is a test asserting it does not happen.

### Corruption

Loading **never throws**. A missing, empty, whitespace-only, malformed, truncated,
non-object, unversioned or future-versioned file all yield an empty queue plus a diagnostic,
and the unusable file is moved aside to `queue.json.corrupt-<timestamp>` rather than deleted.
Losing the queue is survivable; refusing to start is not, and destroying the evidence helps
nobody.

Finer-grained tolerance below that: one unreadable entry among good ones is skipped and
counted rather than costing the whole queue; unknown enum values fall back to safe defaults
(an unrecognized state becomes `FAILED`, never something schedulable); missing fields take
their defaults; an absurd hand-edited `maxConcurrency` is clamped.

A file written by a **newer** build is refused rather than partially read, and left intact —
guessing at fields we do not understand, then writing a downgraded version over the user's
real state, would be worse than starting empty.

### Restart recovery

A job that was executing when the process died becomes **`FAILED` with `E_JOB_INTERRUPTED`**,
not `QUEUED`.

Chosen deliberately. We cannot tell what state its output is in: a half-written download or a
killed encode leaves bytes that may or may not be usable, and neither yt-dlp nor FFmpeg is
being asked to resume. Silently re-queueing risks re-downloading gigabytes without the user
asking, or treating a truncated file as valid input for the next stage of a pipeline. Failing
loudly with a recoverable error puts the decision where it belongs — and a manual retry does
a full artifact sweep first, so it starts clean.

Everything else is restored as it was: queued, waiting, retry-waiting and finished jobs keep
their state, ordering, priority, dependencies and retry counters.

### History retention

Terminal jobs are kept up to `historyLimit` (default 100). Over the limit, the oldest
**non-failed** entry is evicted first, and failures are only dropped once nothing else is
left — a failure is the entry a user actually needs to look at later.

`clearHistory` removes finished entries by scope (completed / failed / cancelled / skipped /
all). It never touches an active or pending job, and it **never deletes a file from disk** —
queue history and media files are separate concerns.

---

## IPC

### Commands added

`getQueueSnapshot`, `setJobPriority`, `moveJob`, `pauseQueue`, `resumeQueue`,
`setConcurrency`, `clearHistory`, `retryFailedJobs`, `removeJob`,
`getProcessingCapabilities`. `createJob` gains the `CONVERSION` and `COMPRESSION` types plus
`priority`, `dependsOn`, `parentJobId`, `retryPolicy` and `allowDuplicate`.

Full shapes in `docs/ipc-contract.md`.

### Validation

Every parameter crosses a trust boundary and is validated rather than read directly: job ids
are length- and control-character-checked, paths reject embedded nulls and `..` segments,
concurrency is bounded to 1–16, enums throw on an unknown value instead of silently
defaulting, and numeric options are range-checked rather than clamped (clamping would produce
output the user did not ask for).

There is no path from IPC to an arbitrary command line. `argv` is always a structured vector,
every codec and container choice comes from a closed enum with a verified recipe, and no
command accepts a format string, a shell fragment, or an FFmpeg argument.

### Events

Added: `jobSkipped`, `jobRetryScheduled`, `queueChanged`. `jobQueued` now also carries the
`WAITING` transition.

**Progress events are throttled** to at most one per job per `progressIntervalMs` (default
200ms), with the latest suppressed value flushed by the scheduler's next pass so a long, quiet
job still shows current progress. A real 25-second encode produced 66 progress events, not
thousands.

### Event ordering

Every event line carries a monotonic `seq`, **stamped as the line is written**, under the
same lock that serializes stdout.

This was a bug first. Stamping at construction time handed out increasing numbers that then
reached the wire out of order, because several threads publish concurrently — caught by the
end-to-end suite, which asserts that arrival order and sequence order are the same thing.
Sequence therefore lives in the IPC layer rather than on `Event`: ordering is a property of
the channel, not of the event.

Each job also carries a `revision` that increments on every durable change. The two together
give the frontend independent guards — `seq` catches a late event outright, `revision` catches
the case where two jobs' events interleave and a newer `seq` does not mean newer information
about *this* job.

`getQueueSnapshot` reads the sequence **before** building the snapshot, so the frontend can
only ever re-apply an event the snapshot already contains (harmless, since events assign
state) rather than skip one it needs.

---

## Frontend

`queueReducer.ts` is a pure reducer with no React in it, which is what makes every
reconciliation rule — out-of-order events, unknown jobs, reconnects, duplicate deliveries —
testable as plain function calls. Components read from it; only the backend writes to it.
`useQueue` subscribes to events *before* fetching the snapshot, so events arriving during the
fetch are applied on top of it rather than lost.

The queue screen shows active, queued, retrying, completed, failed and cancelled work in one
list, with filters, sorting, per-job controls (cancel, retry, move up/down/top, change
priority, inspect, remove), queue controls (pause/resume, concurrency, clear history, retry
all failed), a detail panel, and aggregate statistics.

**No overall progress percentage is shown.** A download measured in bytes and an encode
measured in seconds share no denominator, and averaging them would invent a number.

Controls that cannot work are **disabled with a title explaining why**, never hidden and never
faked. `canCancel` / `canRetry` / `canReorder` / `canChangePriority` are one shared
definition, so a row's buttons and the detail panel cannot disagree.

UX details that are easy to skip and were not: the job row is a real `<button>` so it is
keyboard-reachable with the browser's own focus ring; status colour is always paired with a
text label, never the only signal; long filenames and URLs are truncated in the middle,
keeping both ends, and every text container is `min-width: 0` with `overflow: hidden` so a
300-character filename cannot stretch the layout; progress is clamped to 0–100 so a provider
reporting 101% cannot overflow the track, and a job with no known total shows a neutral bar
rather than a fake moving one; choosing any sort other than "Queue order" displays a note
saying it is a display order only, because otherwise re-sorting would look like it had
re-prioritised the queue.

Error codes map to sentences a person can act on, with the code shown separately as a
diagnostic they can quote. The frontend never parses backend stderr.

---

## Bugs found and fixed during this phase

Each of these was found by a test or a real run, not by inspection.

**Deadlock on retrying a skipped job.** `RetryJob`'s `SKIPPED` branch called
`NotifyQueueChanged()` inside a `lock_guard` scope; `std::mutex` is not recursive. Found by
`RetryingASkippedJobReEvaluatesItsDependencies` hanging.

**Restart recovery contradicted its own policy.** `E_JOB_INTERRUPTED` was classified
transient, so interrupted jobs auto-retried immediately — the exact behaviour the documented
recovery policy ruled out. Found by `InterruptedJobsComeBackAsRetryableFailures`.

**Event sequence numbers reached the wire out of order.** Stamped at construction rather than
at write time. Found by the end-to-end suite; fixed by moving the counter into the stdout
writer.

**The queue state file was never written on Linux.** `DefaultStateFilePath` concatenated a
literal `\`, producing one absurdly-named file instead of a directory. Found by the
end-to-end suite; fixed by building the path through `std::filesystem`.

**Artifact cleanup deleted files it did not create.** `MediaProcessingJob` swept whatever sat
at the output name it wanted, on every attempt — so converting `clip.mp4` into a folder that
already held an unrelated `clip.mp3` would have destroyed the user's file. Found by
`AFirstAttemptNeverDeletesAFileItDidNotCreate`; fixed by keying the sweep on this job's own
previous output.

**Concurrent jobs silently overwrote each other's output.** Deduplication asks the disk "is
this name free?", and two jobs that ask before either has written both get yes. Unreachable at
Phase 1's concurrency of one; routine at Phase 5's. Reproduced six times out of six: three
downloads of the same title, all reporting `COMPLETED`, one file on disk. Fixed with
`OutputNameRegistry`, which reserves rather than merely deduplicates.

**A pipeline stage could not know its input path** — see "The pipeline problem" above.

**Writing a child process's command raced the thread reaping it.** `RealProcessRunner`
documents that only its drain thread may touch the reproc handle, but `WriteLine` calls
`process_.write()` on the caller's thread. When that landed while the drain thread was in
`wait()`, reproc's handles were already closed and the write failed with `EINVAL` rather
than the `EPIPE` the code tolerated — and because `E_PROCESS_WRITE_FAILED` was not
classified transient, the job failed *permanently* on a race it could simply have retried
past. Roughly one download job in eight. Pre-existing, but Phase 5 made it reachable by
launching far more child processes. Fixed on both sides: `WriteLine` now tolerates the
whole "child is already gone" error class (its useful diagnosis is the child's exit code,
which `Wait()` is about to report anyway), and a failed handoff to a child process is now
classified transient. The previously-flaky scenario passes 20 runs out of 20.

---

## Verification matrix

| Area | Automated | Real execution | Evidence |
|---|---|---|---|
| Download | Yes | Yes | `queue_download_e2e.py` §1 — 3 real download jobs, real child processes, ffprobe-verified output |
| Conversion | Yes | Yes | `queue_ffmpeg_e2e.py` §1 — real MP4→MP3, 49KB valid file |
| Compression | Yes | Yes | `queue_ffmpeg_e2e.py` §1 — real re-encode, ffprobe confirms the requested 120px height |
| Queue scheduling | Yes | Yes | `SchedulerCoreTest` (48), `queue_ffmpeg_e2e.py` §9 |
| Concurrency | Yes | Yes | `queue_ffmpeg_e2e.py` §6 — peak `pgrep -c ffmpeg` never exceeded the limit |
| Cancellation | Yes | Yes | `queue_ffmpeg_e2e.py` §7–8 — running encode killed, child confirmed gone, no partial file |
| Retry | Yes | Yes | `queue_download_e2e.py` §2–5 — real retry with real backoff, budget enforced, permanent errors not retried; the flaky-launch scenario soaked 20/20 |
| Persistence | Yes | Yes | `QueuePersistenceTest` (24), `queue_ffmpeg_e2e.py` §13 — real state file inspected |
| Restart recovery | Yes | Yes | `queue_ffmpeg_e2e.py` §13 — process killed, restarted, job restored and completed |
| Corrupted state | Yes | Yes | `queue_ffmpeg_e2e.py` §14 — truncated file, app starts, file quarantined |
| Dependencies | Yes | Yes | `queue_ffmpeg_e2e.py` §3–4, `queue_download_e2e.py` §6 — full download→convert→compress |
| Duplicate detection | Yes | Yes | `queue_ffmpeg_e2e.py` §5 |
| IPC | Yes | Yes | `queue_ffmpeg_e2e.py` §10–11 — 15 malformed inputs rejected, event ordering asserted |
| Frontend logic | Yes | n/a | 57 vitest tests over the reducer and display helpers |
| Frontend build | Yes | Yes | `tsc --noEmit` + `vite build` |
| Rust build | Yes | Yes | `cargo build` |
| Full app launch | Yes | Yes | `mediatool-core` driven over real stdio by both suites |

Not covered, and stated plainly: **yt-dlp's own behaviour against a live URL** — format
selection, extractor quirks, the video/audio merge step. Those are Phase 2 concerns,
unchanged here, and the end-to-end suite deliberately stands yt-dlp in so that retry testing
is reproducible and offline (spec section 50). They need manual verification.

---

## Known limitations

- **No per-job pause for media jobs.** Explained above; the backend reports it honestly
  rather than faking it.
- **No resource-aware scheduling.** One global limit treats a download and a 4K encode as
  equally expensive. The scheduler is structured for it; the policy needs measurements.
- **`BATCH` and `WORKFLOW` job types remain scaffolded.** `createJob` returns an honest
  "not implemented" error.
- **`ExtractAudio` / `ExtractFrames` remain unimplemented** on `IMediaEngine` and say so.
- **Compression is quality-preset based only.** Target-size compression ("get this 2GB file
  under 500MB") needs two-pass encoding and is left for the phase that owns compression
  properly.
- **Duplicate detection is exact-match.** Two requests differing only in an irrelevant
  whitespace difference in a path would not be detected as duplicates. Deliberate: the
  alternative risks merging jobs that are not the same.
- **`OutputNameRegistry` is process-local.** Two instances of the app writing to one folder
  could still collide. The app does not support that, and the ordinary on-disk check still
  applies.
- **Queue state is not shared between app instances.** Last writer wins.
- **The queue UI is functional, not final.** The dark-mode redesign in `docs/roadmap.md`
  remains a later phase.
