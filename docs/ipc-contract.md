# MediaTool IPC & Wire Contract

This is the single source of truth for every name, enum value, and JSON shape that
crosses a process boundary in MediaTool:

```
React/TypeScript  <--Tauri IPC-->  Rust (Tauri shell)  <--stdio NDJSON-->  C++ core (mediatool-core)
                                                                                  |
                                                                     stdio NDJSON |
                                                                                  v
                                                                     Python downloader (downloader.py)
```

Both the `core -> Rust -> TypeScript` hop and the `core -> Python` hop use the **same
framing rules** described below, so there is exactly one protocol to learn, not three.

C++ and TypeScript types MUST mirror this document field-for-field. If you need a field
that isn't listed here, add it here first, then implement it on both sides.

## Naming conventions (do not deviate)

- **Commands and events** (the "verbs" of the protocol): `camelCase`, e.g. `createJob`, `jobProgress`.
- **Enums** (`JobState`, `JobType`, `ErrorCategory`): `UPPER_SNAKE_CASE` string values on the wire,
  e.g. `"RUNNING"`, `"DOWNLOAD"`, `"FILE_NOT_FOUND"`. In C++ these are `enum class` values
  (`JobState::Running`); in TypeScript they are string literal union types (`"RUNNING"`).
- **JSON object fields**: `camelCase`, e.g. `processedBytes`, `etaSeconds`, `jobId`.
- **Timestamps**: ISO-8601 UTC strings, e.g. `"2026-08-23T14:03:11.512Z"`.
- **Job IDs**: opaque strings, format `job-<uuid4>`. Never assume numeric or sortable.

## Framing: NDJSON over stdio

Every message, in both directions, is exactly one JSON object terminated by `\n` (UTF-8,
no embedded raw newlines). No length prefixes, no multi-line pretty-printing on the wire.

### Core process (`mediatool-core`) <-> Rust shell

**stdin (Rust -> core), a request:**
```json
{"id": "req-1", "command": "createJob", "params": {"type": "TEST", "params": {}}}
```

**stdout (core -> Rust), a response** (correlated by `id`, exactly one per request):
```json
{"id": "req-1", "ok": true, "result": {"jobId": "job-3f2a..."}}
```
```json
{"id": "req-1", "ok": false, "error": {"code": "E_NOT_FOUND", "category": "FILE_NOT_FOUND", "message": "...", "details": "...", "recoverable": false}}
```

**stdout (core -> Rust), an unsolicited event** (no `id`; distinguished by the `event` key):
```json
{"event": "jobProgress", "jobId": "job-3f2a...", "timestamp": "2026-08-23T14:03:11.512Z", "data": {"percentage": 42.5, "statusMessage": "Downloading..."}}
```

The Rust shell tells requests from events apart by checking for the `id` key vs. the
`event` key — never by message order. Requests may complete out of order.

### Python downloader (`downloader.py`) <-> C++ core

Same NDJSON framing, but simpler: the core writes **one command line to stdin** when it
starts the process (arguments could also be passed via argv; stdin is preferred so the
process can later be extended to accept follow-up commands, e.g. cancel), and the Python
process streams **event lines to stdout** until it exits. There is no `id`/response
correlation here — one process instance == one logical operation.

Full detail (every field, every error code, filename-handling responsibilities) lives in
**`docs/protocols/downloader.md`** — this section is just the shape summary:

```json
{"command": "inspect", "params": {"url": "..."}}
{"command": "download", "params": {"url": "...", "outputDir": "...", "formatSelector": "...", "filenameBase": "...", "ffmpegLocation": "..."}}
```
```json
{"event": "metadata", "data": {"title": "...", "uploader": "...", "duration": 123, "webpageUrl": "...", "thumbnailUrl": "...", "extractor": "...", "playlistIndex": null, "playlistCount": null, "formats": [...]}}
{"event": "progress", "data": {"downloadedBytes": 1048576, "totalBytes": 52428800, "speedBytesPerSecond": 2097152, "etaSeconds": 24, "statusMessage": "Downloading"}}
{"event": "completed", "data": {"outputPath": "D:\\Videos\\My Video.mp4"}}
{"event": "error", "data": {"code": "E_NETWORK", "category": "NETWORK_ERROR", "message": "...", "details": "...", "recoverable": true}}
```

`formatSelector` is a concrete yt-dlp selector string chosen by
`engines/downloader/YtDlpFormatSelector.h` from a `QualityPreset` — `downloader.py` never
interprets a quality preset itself (spec section 10). `downloader.py --selftest` emits a
canned sequence of these exact events with no network access, so the protocol can be
verified without hitting a real URL.

## Commands (React -> ... -> core)

| command | params | result |
|---|---|---|
| `createJob` | `{type, params, priority?, dependsOn?, parentJobId?, retryPolicy?, allowDuplicate?}` | `{jobId: string, duplicateKey: string}` |
| `getJob` | `{jobId: string}` | `{job: JobSnapshot}` |
| `listJobs` | `{}` | `{jobs: JobSnapshot[]}` |
| `getQueueSnapshot` | `{}` | `{queue: QueueSnapshot}` |
| `cancelJob` | `{jobId: string}` | `{}` |
| `pauseJob` | `{jobId: string}` | `{}` |
| `resumeJob` | `{jobId: string}` | `{}` |
| `retryJob` | `{jobId: string}` | `{}` |
| `removeJob` | `{jobId: string}` | `{}` |
| `setJobPriority` | `{jobId: string, priority: JobPriority}` | `{}` |
| `moveJob` | `{jobId: string, direction: MoveDirection}` | `{}` |
| `pauseQueue` | `{}` | `{runState: "PAUSED"}` |
| `resumeQueue` | `{}` | `{runState: "RUNNING"}` |
| `setConcurrency` | `{maxConcurrency: number}` (1–16) | `{maxConcurrency: number}` |
| `clearHistory` | `{scope?: HistoryScope}` (default `"ALL"`) | `{removedJobIds: string[], removedCount: number}` |
| `retryFailedJobs` | `{}` | `{retriedJobIds: string[], retriedCount: number}` |
| `getProcessingCapabilities` | `{}` | `{targetFormats, compressionPresets, priorities, ffmpegAvailable}` |
| `inspectFile` | `{path: string}` | `{fileInfo: FileInfo}` |
| `inspectDownloadUrl` | `{url: string}` | `{metadata: DownloadMetadata}` |
| `getCapabilities` | `{path: string}` | `{capabilities: string[]}` |
| `getSettings` | `{}` | `{settings: Settings}` |
| `updateSettings` | `{settings: object}` (partial) | `{settings: Settings}` |
| `getHardwareInfo` | `{}` | `{hardwareInfo: HardwareInfo}` |

Unknown commands return `ok: false` with `error.category = "UNKNOWN"`.

**Every parameter is validated, not trusted.** Job ids are length- and
control-character-checked; paths reject embedded nulls and `..` segments; numeric options
are range-checked and **rejected rather than clamped** (clamping would produce output the
user did not ask for); enum values throw on anything unrecognized rather than falling back
to a default. There is no path from IPC to an arbitrary command line: `argv` is always a
structured vector and every codec/container choice comes from a closed enum.

### `createJob` params by `type`

| `type` | `params` |
|---|---|
| `"DOWNLOAD"` | `{url: string, outputDirectory: string, quality?: QualityPreset}` (`quality` defaults to `"BEST"`) |
| `"CONVERSION"` | `{outputDirectory: string, targetFormat: TargetFormat, inputPath? \| inputFromJobId?, outputFilenameBase?: string, maxHeight?: 16–8192, audioBitrateKbps?: 8–2048, gifFps?: 1–60}` |
| `"COMPRESSION"` | `{outputDirectory: string, inputPath? \| inputFromJobId?, preset?: CompressionPreset, outputFilenameBase?: string, outputExtension?: string, maxHeight?: 16–8192, audioBitrateKbps?: 8–2048}` |
| `"TEST"` | `{}` |
| `"BATCH"`, `"WORKFLOW"` | rejected with `error.code = "E_JOB_TYPE_NOT_IMPLEMENTED"` — declared in the `JobType` vocabulary for future phases, not runnable yet |

Exactly one of `inputPath` and `inputFromJobId` must be given for a processing job; sending
both is rejected.

`inputFromJobId` names the job whose output this one consumes. The backend resolves the
actual path once that job has completed, and declaring it **implies the dependency**. This is
how a pipeline is built: the producing job's filename is not knowable in advance — a
download's comes from the media's title and whichever container the extractor chose — so
naming a path there would be a guess. See `docs/phase-5.md` → "The pipeline problem".

### Scheduling options (any `createJob`)

| field | meaning |
|---|---|
| `priority` | `JobPriority`, default `"NORMAL"`. Affects pending order only; never preempts a running job. |
| `dependsOn` | `string[]`, max 32. Unknown ids, self-references, duplicates and cycles are rejected. |
| `parentJobId` | Groups a job under another for display. Carries no scheduling meaning. |
| `retryPolicy` | `{maxRetries?: 0–20, initialDelayMs?, maxDelayMs?, multiplier?: 1.0–10.0}` |
| `allowDuplicate` | `boolean`. When false (default), an identical pending request is rejected with `E_DUPLICATE_JOB` and the existing job's id in `details`. |

## Events (core -> ... -> React)

Every event line carries a monotonic **`seq`**, stamped as the line is written under the
same lock that serializes stdout — so sequence order and arrival order are the same thing.
A client may safely discard any event whose `seq` is not greater than the highest it has
applied.

`jobCreated`, `jobQueued`, `jobStarted`, `jobProgress`, `jobPaused`, `jobResumed`,
`jobCompleted`, `jobFailed`, `jobCancelled`, `jobSkipped`, `jobRetryScheduled` — all carry
`jobId` and a `data` object that is at minimum `{state: JobState}`, plus the job's
`revision` where known. `jobProgress` additionally carries the full `Progress` object.

- `jobQueued` also reports the `WAITING` transition; its `data.state` distinguishes them.
- `jobSkipped` — `data: {state: "SKIPPED", error?: ErrorInfo}` (`E_DEPENDENCY_FAILED`)
- `jobRetryScheduled` — `data: {state: "RETRY_WAIT", attempt, delayMs, reason, maxRetries?, nextRetryAtMs?, error?}`
- `queueChanged` — `data: {runState, maxConcurrency, statistics, pendingOrder}`. No `jobId`.

`revision` increments on every durable change to a job. Use it alongside `seq`: `seq`
catches a late event outright, `revision` catches the case where two jobs' events interleave
and a newer `seq` does not mean newer information about *this* job.

**Progress events are throttled** to at most one per job per ~200ms and coalesced, so a
client will not receive one per FFmpeg tick.

`fileDetected` — `data: {fileInfo: FileInfo}`
`hardwareDetected` — `data: {hardwareInfo: HardwareInfo}`
`downloadMetadataReceived` — `data: {jobId, title, durationSeconds, playlistIndex?, playlistCount?}`
`logEvent` — `data: {level: "DEBUG"|"INFO"|"WARNING"|"ERROR", message: string, subsystem: string}`

## Shared types

### `JobType` (UPPER_SNAKE_CASE)
`"DOWNLOAD" | "CONVERSION" | "COMPRESSION" | "BATCH" | "WORKFLOW" | "TEST"`

`TEST` is a Phase-1-only synthetic job used to prove the pipeline end-to-end; it is not a
real user-facing feature.

### `JobState` (UPPER_SNAKE_CASE)
`"QUEUED" | "WAITING" | "STARTING" | "RUNNING" | "PAUSED" | "RETRY_WAIT" | "COMPLETED" | "FAILED" | "CANCELLED" | "SKIPPED" | "RETRYING"`

- `WAITING` — blocked: a dependency has not completed successfully yet.
- `RETRY_WAIT` — an automatic retry is scheduled; waiting out its backoff.
- `SKIPPED` — will never run: a dependency failed or was cancelled. Distinct from `FAILED`
  because nothing about this job itself went wrong.

Valid transitions (enforced by `JobStateMachine`, all others rejected):
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

`FAILED -> RETRY_WAIT` is the automatic path; `FAILED -> RETRYING` is the manual Retry
button, which skips the backoff. `SKIPPED -> WAITING` is a manual retry of a skipped job: it
re-enters dependency evaluation rather than jumping straight to runnable.

### `JobPriority`, `MoveDirection`, `HistoryScope`, `QueueRunState`
```
JobPriority   "LOW" | "NORMAL" | "HIGH"
MoveDirection "TOP" | "UP" | "DOWN" | "BOTTOM"
HistoryScope  "COMPLETED" | "FAILED" | "CANCELLED" | "SKIPPED" | "ALL"
QueueRunState "RUNNING" | "PAUSED"
```

### `TargetFormat`, `CompressionPreset`
```
TargetFormat      "MP4" | "MKV" | "WEBM" | "MOV" | "GIF" | "MP3" | "WAV" | "M4A" | "FLAC" | "OPUS"
CompressionPreset "LOW" | "MEDIUM" | "HIGH"
```
Closed sets: every entry has a verified `argv` recipe in `FFmpegArgumentBuilder`. Call
`getProcessingCapabilities` to read them from the backend rather than hardcoding a copy.

### `QueueSnapshot`
```ts
{
  runState: QueueRunState;
  maxConcurrency: number;
  statistics: {
    running, queued, waiting, retryWait, paused,
    completed, failed, cancelled, skipped, total: number;
  };
  jobs: JobSnapshot[];
  pendingOrder: string[];   // authoritative scheduling order for pending jobs
  sequence: number;         // this snapshot already reflects every event up to here
}
```

`statistics` carries **no overall percentage**, deliberately: a download measured in bytes
and an encode measured in seconds share no denominator, so any single number would be
invented.

`sequence` is read *before* the snapshot is built, so applying only events with
`seq > sequence` can at worst re-apply something already included (harmless, since events
assign state) rather than skip something needed.

### `Progress`
```ts
{
  percentage?: number;          // 0-100
  processedBytes?: number;
  totalBytes?: number;
  speedBytesPerSecond?: number;
  etaSeconds?: number;
  currentItem?: string;         // e.g. "file 43 of 100"
  statusMessage: string;        // always present, human-readable
}
```

### `ErrorInfo`
```ts
{
  code: string;            // short machine token, e.g. "E_FFMPEG_LAUNCH_FAILED"
  category: ErrorCategory; // see below
  message: string;         // user-facing
  details: string;         // developer diagnostics (stderr, exception text, etc.)
  recoverable: boolean;
}
```

`ErrorCategory` (UPPER_SNAKE_CASE): `"FILE_NOT_FOUND" | "INVALID_FILE" |
"UNSUPPORTED_FORMAT" | "ENGINE_FAILURE" | "DOWNLOAD_FAILURE" | "NETWORK_ERROR" |
"PERMISSION_ERROR" | "DISK_SPACE_ERROR" | "CANCELLED" | "UNKNOWN"`

### `QualityPreset` (UPPER_SNAKE_CASE)
`"BEST" | "2160P" | "1440P" | "1080P" | "720P" | "480P" | "AUDIO_ONLY"`

The only download-quality vocabulary the frontend or the IPC layer ever uses. See
`core/downloads/QualityPreset.h` and `docs/decisions.md` for why the concrete yt-dlp
selector string this maps to is decided in C++, not in Python or the frontend.

### `DownloadFormat`
```ts
{
  formatId: string;
  extension?: string;
  resolution?: string;        // e.g. "1920x1080", video formats only
  width?: number;
  height?: number;
  fps?: number;
  videoCodec?: string;
  audioCodec?: string;
  videoBitrateKbps?: number;
  audioBitrateKbps?: number;
  filesizeBytes?: number;
  approxFilesizeBytes?: number;
  hasVideo: boolean;
  hasAudio: boolean;
}
```

### `DownloadMetadata`
```ts
{
  title: string;
  uploader?: string;
  durationSeconds?: number;
  webpageUrl?: string;
  thumbnailUrl?: string;
  extractor?: string;
  playlistIndex?: number;
  playlistCount?: number;
  formats: DownloadFormat[];  // populated by inspectDownloadUrl; empty from mid-download events
}
```

### `FileInfo`
```ts
{
  path: string;
  filename: string;
  extension: string;
  category: FileCategory; // "VIDEO"|"AUDIO"|"IMAGE"|"DOCUMENT"|"TEXT"|"ARCHIVE"|"UNKNOWN"
  sizeBytes: number;
  mimeType?: string;
  durationSeconds?: number;
  width?: number;
  height?: number;
  videoCodec?: string;
  audioCodec?: string;
  bitrate?: number;
  fps?: number;
}
```

### `HardwareInfo`
```ts
{
  cpu: { name: string; logicalCores: number };
  gpus: Array<{ vendor: "NVIDIA"|"AMD"|"INTEL"|"UNKNOWN"; name: string }>;
  availableEncoders: string[]; // e.g. ["H264_NVENC"] - best-effort, may be empty
}
```

### `Settings`

Grouped exactly as in the product spec: `general`, `downloads`, `processing`, `privacy`,
`advanced`. See `core/settings/Settings.h` for the authoritative field list — mirror it in
`app/frontend/src/types/settings.ts`.

## Job snapshot (`getJob` / `listJobs` / `getQueueSnapshot`)

One shape for **every** job type. A download, a conversion and a compression all arrive
looking like this, so no client has to branch on job type just to read state, progress or
scheduling fields.

```ts
{
  id: string;
  type: JobType;
  state: JobState;
  createdAt: string;
  startedAt?: string;
  completedAt?: string;
  progress: Progress;
  error?: ErrorInfo;
  result?: object;            // e.g. {outputPath, fileInfo}
  metadata?: object;          // descriptive; see below

  // Scheduling
  priority: JobPriority;
  attempt: number;            // attempts beyond the first
  retryCount: number;         // same number, under the spec's name for it
  maxRetries: number;
  nextRetryAtMs?: number;     // present in RETRY_WAIT
  retryReason?: string;       // why the retry decision went the way it did
  dependencies: string[];
  parentJobId?: string;
  queuePosition?: number;     // index in pendingOrder; absent when not pending
  revision: number;           // increments on every durable change
}
```

### `metadata` by job type

Describes **what the job does**, in the job type's own terms. It never contains an FFmpeg
command line, a yt-dlp invocation, or raw stderr — those are implementation details, and a
client that displayed them would be showing the user something meaningless.

| type | fields |
|---|---|
| `DOWNLOAD` | `title`, `uploader?`, `durationSeconds?`, `webpageUrl?`, `thumbnailUrl?` |
| `CONVERSION` | `operation: "CONVERSION"`, `inputPath`, `inputFilename`, `sourceFormat`, `outputPath`, `outputFilename`, `targetFormat`, `targetFormatName`, `audioOnly`, `maxHeight?`, `audioBitrateKbps?`, `gifFps?` |
| `COMPRESSION` | `operation: "COMPRESSION"`, `inputPath`, `inputFilename`, `sourceFormat`, `outputPath`, `outputFilename`, `targetFormat`, `preset`, `maxHeight?`, `audioBitrateKbps?` |

## Versioning

This contract is version `0` (pre-1.0, foundation phase). Every message MAY grow new
optional fields without a version bump. A breaking change (renaming/removing a field,
changing an enum's wire representation) requires bumping a `protocolVersion` field to be
added to the handshake in a later phase — not needed yet, but do not design against that
possibility (e.g. don't positionally-index arrays where a keyed object would do).

## Durable queue state

Separate from this wire contract, and versioned separately: the queue's own state file at
`%LOCALAPPDATA%\MediaTool\queue.json` carries `schemaVersion` (currently `1`). Bump it for
an incompatible on-disk change and add a migration.

The reader is deliberately forgiving in one direction and strict in the other. Missing
fields take defaults, unknown enum values fall back to something safe (an unrecognized state
becomes `FAILED`, never something schedulable), and one unreadable entry among good ones is
skipped rather than costing the whole queue. But a file whose `schemaVersion` is **newer**
than this build understands is refused outright and left intact — reading it would mean
guessing at fields we do not understand, and then writing a downgraded version over the
user's real state.

Anything unusable is moved to `queue.json.corrupt-<timestamp>` rather than deleted, and the
app starts with an empty queue. See `docs/phase-5.md` → "Persistence and recovery".
