# Gravity IPC & Wire Contract

This is the single source of truth for every name, enum value, and JSON shape that
crosses a process boundary in Gravity:

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
| `createJob` | `{type: JobType, params: object}` | `{jobId: string}` |
| `listJobHistory` | `{limit?: number}` | `{jobs: JobSnapshot[]}` (most-recent-first; backed by `job_history.json`, a bounded ring buffer of terminal-state jobs -- see `core/jobs/JobHistoryStore.h`) |
| `getJob` | `{jobId: string}` | `{job: JobSnapshot}` |
| `listJobs` | `{}` | `{jobs: JobSnapshot[]}` |
| `cancelJob` | `{jobId: string}` | `{}` |
| `pauseJob` | `{jobId: string}` | `{}` |
| `resumeJob` | `{jobId: string}` | `{}` |
| `retryJob` | `{jobId: string}` | `{}` |
| `inspectFile` | `{path: string}` | `{fileInfo: FileInfo}` |
| `inspectDownloadUrl` | `{url: string}` | `{metadata: DownloadMetadata}` |
| `getCapabilities` | `{path: string}` | `{capabilities: string[], deferredCapabilities: {capability: string, reason: string}[]}` |
| `getSettings` | `{}` | `{settings: Settings}` |
| `updateSettings` | `{settings: object}` (partial) | `{settings: Settings}` |
| `getHardwareInfo` | `{}` | `{hardwareInfo: HardwareInfo}` |

Unknown commands return `ok: false` with `error.category = "UNKNOWN"`.

### Request handling guarantees

- **Every request with a usable `id` gets exactly one response.** A request whose `id` is
  missing, non-string or empty is logged and dropped instead -- there is no pending caller
  to route an answer to, and writing an unroutable line would leave whoever sent it waiting
  out the full 30s bridge timeout for nothing.
- **A request line is limited to 1 MiB.** Anything longer is discarded up to the next
  newline and logged; the loop keeps reading. See `core/ipc/LineReader.h`.
- **Parameters are validated before the handler runs** (`core/ipc/RequestValidation.h`).
  A missing field is `E_MISSING_PARAM`, a wrong type is `E_INVALID_PARAM_TYPE`, and an
  out-of-range or disallowed value is `E_INVALID_PARAM_VALUE`; each names the offending
  field in `error.details`. An explicit JSON `null` is treated as an absent field
  throughout.
- **`inspectDownloadUrl` and `inspectFile` run off the request loop**, on a bounded pool
  (`core/ipc/RequestExecutor.h`), so a slow network lookup cannot delay other commands.
  Responses are still correlated by `id`; as stated above, requests may complete out of
  order. If that pool is saturated the request is answered with `E_CORE_BUSY`
  (`recoverable: true`), never queued without limit.
- **`inspectDownloadUrl` and `createJob{type: "DOWNLOAD"}` check that the Python downloader
  is actually present** before doing anything else, and answer `E_DOWNLOADER_NOT_FOUND`
  with every candidate path that was tried in `error.details` if it isn't (issue #79).
  A missing interpreter does not stop the core from starting or from running conversion,
  compression and settings commands.

### `createJob` params by `type`

| `type` | `params` |
|---|---|
| `"DOWNLOAD"` | `{url: string, outputDirectory: string, quality?: QualityPreset, formatId?: string, priority?: number, dependsOn?: string[]}` (`quality` defaults to `"BEST"`; `formatId` — an exact stream id, or `"id1+id2"` combo, from `inspectDownloadUrl`'s format list — overrides `quality` entirely when set, issue #31, and is validated before the job is accepted: see "Format id validation" below) |
| `"CONVERSION"` / `"COMPRESSION"` | `{inputPath: string, outputDirectory: string, options: MediaProcessingOptions, priority?: number, dependsOn?: string[]}` — see below. `inputPath`/`outputDirectory` are validated the same way as DOWNLOAD's `outputDirectory` (absolute, no `..` segments, UNC rejected unless `advanced.allowNetworkPaths` is set). |
| `"TEST"` | `{priority?: number, dependsOn?: string[]}` |
| anything else | rejected with `error.code = "E_JOB_TYPE_NOT_IMPLEMENTED"` — declared in the `JobType` vocabulary for future phases, not runnable yet |

#### Scheduling params (every job type)

Both are optional, and both are interpreted by `core/jobs/SchedulerCore.h` — see
`docs/concurrency-model.md` for the full ordering and dependency semantics.

| param | meaning |
|---|---|
| `priority` | Integer in `[-1000, 1000]`, default `0`. Higher runs first among queued jobs; ties keep submission (FIFO) order. Out of range → `E_INVALID_PARAM_VALUE`. |
| `dependsOn` | Up to 32 job ids that must reach `COMPLETED` before this job may start. Each must be a job the core already knows about — an unknown id, this job itself, or a job that already ended in `FAILED`/`CANCELLED` is rejected at submission with `E_INVALID_DEPENDENCY`. Because a dependency must already exist, dependency cycles cannot be expressed. |

A job whose dependency does not complete is **cancelled**, transitively down the chain:
its `state` becomes `CANCELLED` with no `error` field, and the reason is in the core log.
`removeJob` on a job that a still-queued job depends on is rejected with
`E_JOB_HAS_DEPENDENTS`.

#### `MediaProcessingOptions` (CONVERSION/COMPRESSION's `options`)

`{outputFormat: string, quality?: "low"|"medium"|"high"|"lossless", videoCodec?: "auto"|"h264"|"h265"|"vp9"|"av1", hardwareAcceleration?: "auto"|"none"|"nvenc"|"amf"|"qsv", resolution?: {width: number, height: number}, trim?: {startSeconds?: number, endSeconds?: number}, watermark?: {imagePath: string, position: "top-left"|"top-right"|"bottom-left"|"bottom-right"|"center", opacity: number}, audioBitrateKbps?: number}`

`outputFormat` is required (rejected with `E_MISSING_PARAM` if absent, `E_INVALID_PARAM_VALUE` if empty). `quality`, when present, must be one of `lowest` | `low` | `medium` | `high` | `ultra` | `lossless` — anything else is rejected with `E_INVALID_PARAM_VALUE` rather than silently degraded to `medium` as it was before (issue #83). `lossless` used to be rejected unconditionally as a Pro-tier value; issue #82 removed the tier, so it is now an ordinary selectable quality. See `engines/ffmpeg/FFmpegArgBuilder.h` for exactly how each field maps to ffmpeg arguments (encoder selection, CRF tiers, the GIF palette pipeline, image-format handling, trim/watermark filter graphs).

A `COMPRESSION` job additionally probes its input before encoding and derives an explicit video (or, for an audio-only target, audio) bitrate from the source's own bitrate, scaled by the quality tier. This is what makes compression mean "smaller than the input" rather than "re-encoded at a fixed perceptual quality" — the latter routinely produced a *larger* file, which is issue #80. Callers do not supply the target and cannot override it; an explicit `audioBitrateKbps` is still honoured as-is.

#### Format id validation

`formatId` reaches yt-dlp's `-f` verbatim, and `-f` is a small expression language
(filters, fallbacks, arithmetic, `all`), not a name. It is therefore held to the shape of
an actual format id: up to eight `+`-joined ids, each 1–64 characters of `[A-Za-z0-9_.-]`,
with `all` and `mergeall` rejected by name because they change how many streams get
downloaded. Anything else is rejected with `E_INVALID_FORMAT_ID`
(`category: "UNSUPPORTED_FORMAT"`) synchronously from `createJob`, before any process is
started. The provider re-checks the same rule immediately before spawning, so every path
into `-f` is covered. The selectors derived from `QualityPreset` are built in C++ and are
deliberately *not* subject to this rule — they legitimately use the expression syntax.

#### Deferred operations

`getCapabilities` answers two disjoint lists. Everything in `capabilities` will be
attempted for real. Everything in `deferredCapabilities` applies to the file but cannot
run in this build; each entry carries a `reason` that is user-facing and safe to render
verbatim next to a disabled control. Attempting a deferred operation fails with
`E_NOT_IMPLEMENTED` (`category: "UNSUPPORTED_FORMAT"`, `recoverable: false`) and never
partially runs, at every layer, with `message` equal to that same `reason`.

Today that means `extractAudio` and `extractFrames` on video files. They used to be
reported as ordinary capabilities, so the only way to discover they do not run was to
start a job and read the failure. `core/media/DeferredOperations.h` is the single table
both lists are derived from.

#### Downloader failure codes

Beyond the download failures already classified (`E_VIDEO_PRIVATE`, `E_GEO_RESTRICTED`,
`E_FORMAT_UNAVAILABLE`, …), the merge/post-processing stage — yt-dlp's own ffmpeg step,
which runs *after* the bytes are on disk — reports:

| code | category | `recoverable` | meaning |
|---|---|---|---|
| `E_MERGE_TOOL_MISSING` | `ENGINE_FAILURE` | `false` | yt-dlp could not find ffmpeg/ffprobe to merge the streams |
| `E_MERGE_FAILED` | `ENGINE_FAILURE` | `false` | the merge or a post-processor ran and failed |
| `E_FRAGMENT_DOWNLOAD_FAILED` | `NETWORK_ERROR` | `true` | a fragment could not be transferred |
| `E_FRAGMENT_MISSING` | `DOWNLOAD_FAILURE` | `false` | a fragment the manifest names does not exist |

`E_INSPECT_TIMEOUT` (`NETWORK_ERROR`, `recoverable: true`) is reported when a metadata
fetch exceeds its wall-clock deadline (60s by default). This is a bound on the *caller*,
which yt-dlp's own `socket_timeout` is not: that limits one connect/read, while a fetch is
many of them and a child that stalls without any single socket call timing out would
otherwise hold a worker thread indefinitely. The child is stopped before the error is
reported. Downloads deliberately have no such deadline — a legitimately long download is
not a hang.

### Automatic retry

A job that fails is retried automatically when — and only when — **both** of these hold:

* the layer that produced the failure set `error.recoverable`, and
* the failure's category is not one where a second attempt cannot help. `FILE_NOT_FOUND`,
  `INVALID_FILE`, `UNSUPPORTED_FORMAT`, `PERMISSION_ERROR`, `DISK_SPACE_ERROR` and
  `CANCELLED` are never retried, whatever `recoverable` says.

Either signal alone gets a class of failures wrong, which is why both are required: a
provider that mislabels a full disk as recoverable would otherwise turn one clear failure
into three identical ones seconds apart, and a `NETWORK_ERROR` for a hostname that does not
resolve fails the same way forever. See `core/jobs/RetryPolicy.h`.

The budget is `settings.processing.maxRetryAttempts` (1–10, default 3), counting the first
attempt — so `1` disables retry entirely. Waits are exponential (2s, then 4s, …, capped at
60s). A retried job transitions `RUNNING → RETRYING → RUNNING`, **never through `FAILED`**:
entering a terminal state would cancel every job that `dependsOn` it and write a failure
into `listJobHistory`, both for an attempt that is about to be repeated. Only a job that has
exhausted its attempts, or failed in a way that is not retryable, reaches `FAILED`.

A job sitting out a backoff is fully cancellable — `cancelJob` finalizes it immediately
rather than waiting out the timer.

Every job snapshot carries `attempts`: the attempts that have already run, including the
one in progress. Compare it against `settings.processing.maxRetryAttempts` to render
"attempt 2 of 3". The count survives a restart, so a job rebuilt by crash recovery does not
get a fresh budget.

### Crash recovery

Unfinished jobs are persisted as the `createJob` request that produced them and replayed on
the next launch through the same submission path a fresh request takes. The guarantee is
deliberately the weak one: **a job interrupted by a crash is rebuilt and re-run from the
start, not resumed** — its subprocess died with the process, and neither yt-dlp nor ffmpeg
offers a resume protocol to drive. Artifacts the killed run left in the output directory are
deleted before the rebuilt job starts.

A rebuilt job is a new `Job` and therefore has a **new job id**; `dependsOn` edges are
remapped across the replay, and an edge naming a job that already finished is dropped
(it is either satisfied or unsatisfiable). Recovery is capped at three attempts per job, so
a job that takes the core down cannot re-queue itself on every launch.

## Events (core -> ... -> React)

**Actually emitted today:**

`jobCreated`, `jobStarted`, `jobProgress`, `jobPaused`, `jobResumed`, `jobCompleted`,
`jobFailed`, `jobCancelled` — all carry `jobId` and a `data` object that is at minimum
`{state: JobState}` and, for `jobProgress`, the full `Progress` object.

`jobRetrying` — `data: {state: "RETRYING", attempt: number, maxAttempts: number,
retryInMs: number, error: ErrorInfo}`. **Not a terminal event and deliberately not a
`jobFailed`:** the attempt failed, the job did not. A job that will be retried never enters
`FAILED` at all (see "Automatic retry" below), so a listener that treats `jobFailed` as
"this job is over" stays correct.

`logEvent` — `data: {level: "DEBUG"|"INFO"|"WARNING"|"ERROR", message: string, subsystem: string}`,
forwarded from `Logger::SetEventSink` (`app/core/main.cpp`).

**Declared in `EventType`/`CoreEventName` (both sides), never actually published anywhere
in the codebase** (confirmed via a repo-wide grep for each event's construction site, not
just its name string in `Event.cpp`'s `ToWireString` — issue #22). Treat these as reserved,
not implemented; a frontend listener for any of them will simply never fire:

- `jobQueued` — `data: {state: "QUEUED"}`
- `fileDetected` — `data: {fileInfo: FileInfo}`
- `hardwareDetected` — `data: {hardwareInfo: HardwareInfo}`
- `downloadMetadataReceived` — `data: {jobId, title, durationSeconds, playlistIndex?, playlistCount?}`

## Rust-only Tauri commands & events (Phase 4, not through the C++ core)

OS-shell features (tray/background mode, Watch Folders, Scheduled Tasks, global Hotkeys)
live entirely in `app/desktop/src-tauri/src/` and never get a `mediatool-core` command of
their own -- when one needs to create a job, it just calls the existing `createJob` verb
above through the same `CoreState::send_request` the `send_core_command` bridge uses.
These commands are invoked directly (`invoke("command_name", params)`), not through
`send_core_command`.

| command | params | result | source |
|---|---|---|---|
| `add_watch_folder` | `{path: string, jobType: "CONVERSION"\|"COMPRESSION", defaultOptions: object}` | `{}` | `watch_folders.rs` |
| `remove_watch_folder` | `{path: string}` | `{}` | `watch_folders.rs` |
| `list_watch_folders` | `{}` | `WatchFolderConfig[]` (`{path, jobType, defaultOptions}`) | `watch_folders.rs` |
| `add_scheduled_task` | `{name: string, cronExpression: string, jobType: "CONVERSION"\|"COMPRESSION"\|"DOWNLOAD", params: object}` | `ScheduledTaskConfig` | `scheduler.rs` |
| `update_scheduled_task` | `{id: string, name?, cronExpression?, enabled?, params?}` | `ScheduledTaskConfig` | `scheduler.rs` |
| `remove_scheduled_task` | `{id: string}` | `{}` | `scheduler.rs` |
| `list_scheduled_tasks` | `{}` | `ScheduledTaskConfig[]` (`{id, name, cronExpression, jobType, params, enabled}`) | `scheduler.rs` |
| `refresh_hotkeys` | `{}` | `{}` | `hotkeys.rs` -- call after any `updateSettings` that could have changed `general.hotkeyPasteAndDownload`/`hotkeyFocusQueue` |
| `get_startup_file_action` | `{}` | `{path: string, mode: "convert"\|"compress"} \| null` | `cli.rs` -- call once on frontend mount to fetch-and-consume a `--convert`/`--compress` path Gravity was launched with (Phase 5.3 Windows context menu); returns `null` on a normal launch |
| `open_containing_folder` | `{path: string}` | `{}` | `lib.rs` |

`cronExpression` uses the `cron` crate's 6-field syntax (seconds first --
`sec min hour day month day-of-week`), not the 5-field Unix convention.

Desktop notifications (Phase 4.5) are the one exception to "OS-shell features live in
Rust": they fire from the **frontend** via `@tauri-apps/plugin-notification`'s
`sendNotification`/`requestPermission`/`isPermissionGranted` (wrapped in
`app/frontend/src/services/notifications.ts`), gated by `general.showNotifications`, in
response to the job lifecycle events above plus the two background-trigger events below --
not a Tauri command of its own, since React already subscribes to everything it needs to
react to.

Events emitted directly by Rust (via `AppHandle::emit`, subscribed with
`@tauri-apps/api/event`'s `listen`, not `subscribeToJobEvents`'s `"core-event"`):

| event | payload | fired by |
|---|---|---|
| `watch-folder-triggered` | `{path: string, jobId?: string}` | a watched folder auto-submitted a job |
| `scheduled-task-fired` | `{taskId: string, taskName: string, jobId?: string}` | a scheduled task's cron fired |
| `hotkey-paste-and-download` | `{url: string}` | the paste-and-download hotkey read a non-empty clipboard |
| `hotkey-focus-queue` | `{}` | the focus-queue hotkey fired |
| `cli-file-opened` | `{path: string, mode: "convert"\|"compress"}` | Phase 5.3: a second `gravity.exe --convert`/`--compress "<path>"` launch was redirected to this already-running instance by `tauri-plugin-single-instance` -- the cold-start equivalent is `get_startup_file_action` above, not this event (the frontend isn't mounted/listening yet at cold start) |

## Shared types

### `JobType` (UPPER_SNAKE_CASE)
`"DOWNLOAD" | "CONVERSION" | "COMPRESSION" | "BATCH" | "WORKFLOW" | "TEST"`

`TEST` is a Phase-1-only synthetic job used to prove the pipeline end-to-end; it is not a
real user-facing feature.

### `JobState` (UPPER_SNAKE_CASE)
`"QUEUED" | "STARTING" | "RUNNING" | "PAUSED" | "COMPLETED" | "FAILED" | "CANCELLED" | "RETRYING"`

Valid transitions (enforced by `JobStateMachine`, all others rejected):
```
QUEUED    -> STARTING, CANCELLED
STARTING  -> RUNNING, FAILED, CANCELLED
RUNNING   -> PAUSED, COMPLETED, FAILED, CANCELLED
PAUSED    -> RUNNING, CANCELLED
FAILED    -> RETRYING
RETRYING  -> RUNNING, FAILED
```

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

## Job snapshot (`getJob` / `listJobs` / embedded in job events)

```ts
{
  id: string;
  type: JobType;
  state: JobState;
  // Scheduling priority (issue #17): higher runs before lower among jobs still Queued;
  // FIFO among equal priorities. Defaults to 0 if not set at createJob time.
  priority: number;
  // Attempts that have already run, including the one in progress: 1 on a first run, 2
  // while retrying after one failure. See "Automatic retry" above.
  attempts: number;
  createdAt: string;
  startedAt?: string;
  completedAt?: string;
  progress: Progress;
  error?: ErrorInfo;
  result?: object;
  metadata?: object;
}
```

## Versioning

This contract is version `0` (pre-1.0, foundation phase). Every message MAY grow new
optional fields without a version bump. A breaking change (renaming/removing a field,
changing an enum's wire representation) requires bumping a `protocolVersion` field to be
added to the handshake in a later phase — not needed yet, but do not design against that
possibility (e.g. don't positionally-index arrays where a keyed object would do).
