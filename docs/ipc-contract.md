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
| `getCapabilities` | `{path: string}` | `{capabilities: string[]}` |
| `getSettings` | `{}` | `{settings: Settings}` |
| `updateSettings` | `{settings: object}` (partial) | `{settings: Settings}` |
| `getHardwareInfo` | `{}` | `{hardwareInfo: HardwareInfo}` |

Unknown commands return `ok: false` with `error.category = "UNKNOWN"`.

### `createJob` params by `type`

| `type` | `params` |
|---|---|
| `"DOWNLOAD"` | `{url: string, outputDirectory: string, quality?: QualityPreset}` (`quality` defaults to `"BEST"`) |
| `"CONVERSION"` / `"COMPRESSION"` | `{inputPath: string, outputDirectory: string, options: MediaProcessingOptions}` — see below. `inputPath`/`outputDirectory` are validated the same way as DOWNLOAD's `outputDirectory` (absolute, no `..` segments, UNC rejected unless `advanced.allowNetworkPaths` is set). |
| `"TEST"` | `{}` |
| anything else | rejected with `error.code = "E_JOB_TYPE_NOT_IMPLEMENTED"` — declared in the `JobType` vocabulary for future phases, not runnable yet |

#### `MediaProcessingOptions` (CONVERSION/COMPRESSION's `options`)

`{outputFormat: string, quality?: "low"|"medium"|"high"|"lossless", videoCodec?: "auto"|"h264"|"h265"|"vp9"|"av1", hardwareAcceleration?: "auto"|"none"|"nvenc"|"amf"|"qsv", resolution?: {width: number, height: number}, trim?: {startSeconds?: number, endSeconds?: number}, watermark?: {imagePath: string, position: "top-left"|"top-right"|"bottom-left"|"bottom-right"|"center", opacity: number}, audioBitrateKbps?: number}`

`outputFormat` is required (rejected with `E_INVALID_MEDIA_OPTIONS` if missing/empty). `quality: "lossless"` is rejected unconditionally with `E_PRO_FEATURE_LOCKED` — there is no Pro entitlement system yet, so this is a hard server-side gate, not a toggle; the frontend must never offer it as a selectable value. See `engines/ffmpeg/FFmpegArgBuilder.h` for exactly how each field maps to ffmpeg arguments (encoder selection, CRF tiers, the GIF palette pipeline, image-format handling, trim/watermark filter graphs).

## Events (core -> ... -> React)

`jobCreated`, `jobQueued`, `jobStarted`, `jobProgress`, `jobPaused`, `jobResumed`,
`jobCompleted`, `jobFailed`, `jobCancelled` — all carry `jobId` and a `data` object
that is at minimum `{state: JobState}` and, for `jobProgress`, the full `Progress` object.

`fileDetected` — `data: {fileInfo: FileInfo}`
`hardwareDetected` — `data: {hardwareInfo: HardwareInfo}`
`downloadMetadataReceived` — `data: {jobId, title, durationSeconds, playlistIndex?, playlistCount?}`
`logEvent` — `data: {level: "DEBUG"|"INFO"|"WARNING"|"ERROR", message: string, subsystem: string}`

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
