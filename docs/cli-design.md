# Gravity CLI Design

## Overview

A command-line interface for Gravity that allows users to perform downloads and conversions without the GUI, enabling automation, scripting, and headless operation.

The CLI directly invokes `mediatool-core.exe` (the existing C++ sidecar) over its stdio NDJSON protocol, reusing the exact same backend as the React frontend. No new core logic is written; the CLI is a thin command-line wrapper around the existing job system.

## Architecture

```
CLI (Rust or C++)
  ↓
mediatool-core.exe (existing C++ core)
  ↓
Job system (DownloadJob, MediaProcessingJob)
  ↓
Python downloader / FFmpeg
```

The CLI:
- Spawns `mediatool-core.exe` as a subprocess
- Sends NDJSON commands over stdin (using the existing `docs/ipc-contract.md` protocol)
- Parses NDJSON events from stdout
- Translates them into user-friendly CLI output (progress bars, logs, etc.)
- Returns appropriate exit codes

## Commands

### 1. Download

```bash
gravity download <URL> [OPTIONS]
gravity dl <URL> [OPTIONS]  # short form
```

**Options:**
- `--output-dir <PATH>` — where to save the file (required)
- `--quality <PRESET>` — quality preset: `BEST`, `2160P`, `1440P`, `1080P`, `720P`, `480P`, `AUDIO_ONLY` (default: `BEST`)
- `--playlist-dir <PATH>` — when URL is a playlist, download all entries into this directory (auto-numbered/named)
- `--wait` — block until the job completes (default for CLI: true)
- `--json` — output events as NDJSON instead of human-readable progress
- `--quiet` — minimal output (errors only)

**Examples:**
```bash
gravity download "https://www.youtube.com/watch?v=..." --output-dir ~/Videos
gravity download "https://www.youtube.com/playlist?list=..." --playlist-dir ~/Videos/my-playlist --quality 720P
gravity dl https://youtu.be/... --output-dir . --wait --quiet
```

**Behavior:**
- If the URL is a playlist and `--playlist-dir` is not set, fail with an error suggesting it
- Create the output directory if it doesn't exist
- Show live progress: `[████████░░] 42% (25.3 MB / 60 MB) ETA 2m 15s`
- On completion, print the output file path
- Exit code: 0 on success, non-zero on failure

### 2. Convert

```bash
gravity convert <INPUT_FILE> [OPTIONS]
```

**Options:**
- `--output-dir <PATH>` — where to save the converted file (default: same as input)
- `--output-file <NAME>` — output filename (default: auto-generated from input)
- `--format <FORMAT>` — target format: `MP4`, `MKV`, `WEBM`, `AVI`, `MOV`, `FLAC`, `MP3`, `WAV`, `OGG` (required)
- `--quality <LEVEL>` — `HIGH`, `MEDIUM`, `LOW` (default: `HIGH`)
- `--wait` — block until conversion completes (default: true)
- `--json` — output events as NDJSON
- `--quiet` — minimal output

**Examples:**
```bash
gravity convert video.mkv --format MP4 --output-dir ~/converted
gravity convert audio.wav --format MP3 --quality MEDIUM
```

**Behavior:**
- Validate input file exists and is convertible (via `inspectFile`)
- Show progress: `[██████████] 78% (2m 15s elapsed) ETA 42s`
- On completion, print the output file path
- Exit code: 0 on success, non-zero on failure

### 3. Compress

```bash
gravity compress <INPUT_FILE> [OPTIONS]
```

**Options:**
- `--output-dir <PATH>` — where to save the compressed file (default: same as input)
- `--output-file <NAME>` — output filename (default: auto-generated)
- `--quality <LEVEL>` — target quality: `HIGH`, `MEDIUM`, `LOW` (default: `MEDIUM`)
- `--target-size <SIZE>` — optional: compress to approximately this size (e.g., `100MB`, `1GB`)
- `--wait` — block until compression completes (default: true)
- `--json` — output events as NDJSON
- `--quiet` — minimal output

**Examples:**
```bash
gravity compress large-video.mp4 --quality MEDIUM --target-size 500MB
gravity compress movie.mkv --output-dir ./compressed --quality LOW
```

**Behavior:**
- Similar to `convert`, but optimizes for file size reduction
- If `--target-size` is set, estimate required bitrate and display it
- Show progress with compression ratio
- Exit code: 0 on success, non-zero on failure

### 4. Batch

```bash
gravity batch <JSON_FILE>
```

Accepts a JSON file describing multiple operations to queue and execute.

**JSON format:**
```json
{
  "operations": [
    {
      "type": "download",
      "url": "https://...",
      "outputDir": "~/Videos",
      "quality": "720P"
    },
    {
      "type": "convert",
      "inputFile": "video1.mkv",
      "outputDir": "~/converted",
      "format": "MP4"
    },
    {
      "type": "compress",
      "inputFile": "video2.mp4",
      "quality": "MEDIUM",
      "targetSize": "500MB"
    }
  ],
  "concurrent": 2,
  "stopOnError": false
}
```

**Options:**
- `--wait` — block until all jobs complete (default: true)
- `--json` — output events as NDJSON (one per operation, tagged with order)
- `--quiet` — minimal output

**Example:**
```bash
gravity batch jobs.json --concurrent 3 --wait
```

### 5. Queue Management

```bash
gravity queue list
gravity queue status [JOB_ID]
gravity queue cancel [JOB_ID]
gravity queue pause [JOB_ID]
gravity queue resume [JOB_ID]
gravity queue retry [JOB_ID]
```

**Examples:**
```bash
gravity queue list                    # List all queued/running jobs
gravity queue status job-abc123       # Get status of a specific job
gravity queue cancel job-abc123       # Cancel a job
gravity queue pause job-abc123        # Pause a download/job
gravity queue resume job-abc123       # Resume a paused job
gravity queue list --history --limit 20  # Show 20 most recent completed jobs
```

**Output:**
```
ID                    Type       State      Progress      
job-f1a2...          DOWNLOAD   RUNNING    42% (25/60MB)  
job-b3c4...          CONVERT    QUEUED     —              
job-d5e6...          COMPRESS   FAILED     —
```

## Output Formats

### Human-Readable (default)

```
Downloading: Big Buck Bunny
████████████████░░░░ 76% (380 MB / 500 MB)
Speed: 45 MB/s | ETA: 3m 45s

✓ Saved to: C:\Users\...\Downloads\Big Buck Bunny.mp4
```

### JSON (`--json` flag)

All output is NDJSON (one JSON object per line), matching the existing `docs/ipc-contract.md` event format:

```json
{"event": "jobProgress", "jobId": "job-abc123", "timestamp": "2026-09-07T14:03:11.512Z", "data": {"percentage": 42.5, "statusMessage": "Downloading..."}}
{"event": "jobCompleted", "jobId": "job-abc123", "timestamp": "2026-09-07T14:05:30.123Z", "data": {"exitCode": 0, "result": {"outputPath": "C:\\Videos\\file.mp4"}}}
```

This makes it easy to parse in scripts:
```bash
gravity download https://... --output-dir . --json | jq -r '.data.statusMessage'
```

### Quiet (`--quiet` flag)

Errors only. Successful operations produce zero output.

## Exit Codes

- `0` — Success
- `1` — General error (invalid arguments, unsupported format, etc.)
- `2` — Job failed (file not found, network error, ffmpeg error, etc.)
- `3` — Job cancelled by user
- `4` — Core process error (mediatool-core.exe crashed or is unavailable)
- `5` — Timeout (when using `--wait` with a timeout option)

## Implementation Path

### Phase 1: MVP (downloads + basic queue)

- [x] Spawn `mediatool-core.exe` and communicate over NDJSON
- [ ] Implement `gravity download` command
- [ ] Implement `gravity queue list|status|cancel` commands
- [ ] Implement `--json` and `--quiet` flags
- [ ] Human-readable progress with standard library or single-dependency crate (e.g., `indicatif`)
- [ ] Write integration tests using the existing Python `--selftest` pattern

**Language choice:** Rust (reuses Tauri infrastructure, familiar to the project, excellent CLI libraries like `clap`)

### Phase 2: Conversion + Batch

- [ ] Implement `gravity convert` and `gravity compress` commands
- [ ] Implement `gravity batch` command
- [ ] Add `--target-size` estimation for compression
- [ ] Test against real ffmpeg output

### Phase 3: Polish

- [ ] Shell completions (bash, zsh, PowerShell)
- [ ] Configuration file support (`~/.gravity/config.json`)
- [ ] Preset definitions (e.g., save custom quality profiles)
- [ ] Man page / help text
- [ ] Installer integration (add CLI to PATH)

## Design Principles

1. **No new core logic** — reuse `mediatool-core.exe` exactly as the GUI does. Protocol is already proven.

2. **One command per job type** — `download`, `convert`, `compress`. Simple to learn, hard to get wrong.

3. **Sensible defaults** — if the user omits `--quality`, we pick `BEST` (download) or `MEDIUM` (compress). If they omit `--output-dir`, we prompt or error clearly.

4. **Wait by default** — the CLI blocks until the job completes. This is more intuitive for automation scripts. Add `--no-wait` if the user prefers fire-and-forget, returning a job ID.

5. **JSON for machines, humans for humans** — `--json` is always available for scripting; the default output is readable without `jq`.

6. **Fail visibly** — when something goes wrong, print the error and exit non-zero. Include the job ID so the user can investigate with `gravity queue status`.

## Interaction with mediatool-core

The CLI will:
1. Spawn `mediatool-core.exe` (or find it if it's already running — future enhancement)
2. Send a `createJob` command with the user-specified parameters
3. Poll with `getJob` or subscribe to job events (depending on implementation)
4. Translate stdout NDJSON events into user-facing output
5. Return the appropriate exit code

The C++ core needs no changes — it already speaks NDJSON and knows how to handle jobs.

## Example Workflows

### Download a video
```bash
gravity download "https://www.youtube.com/watch?v=dQw4w9WgXcQ" --output-dir ~/Videos
```

### Download a playlist
```bash
gravity download "https://www.youtube.com/playlist?list=..." --playlist-dir ~/Videos/my-list --quality 720P
```

### Convert a file
```bash
gravity convert ~/Downloads/movie.mkv --format MP4 --output-dir ~/Videos
```

### Compress and monitor
```bash
gravity compress big-file.mp4 --quality MEDIUM --target-size 500MB --json | tee compression.log
```

### Batch job from a config file
```bash
gravity batch batch-jobs.json --concurrent 3
```

### Automation: download + convert
```bash
#!/bin/bash
OUTDIR="./temp"
gravity download "https://www.youtube.com/watch?v=..." --output-dir "$OUTDIR" --quality BEST
FILE=$(ls -t "$OUTDIR" | head -1)
gravity convert "$OUTDIR/$FILE" --format MP4 --output-dir ./final
```

## Non-Goals

- Real-time monitoring of existing long-running jobs (use `gravity queue status` for that)
- Web server mode or daemon mode (out of scope for MVP)
- Custom filtering or advanced yt-dlp options (the CLI exposes high-level presets only)
- GUI for the CLI (defeat the purpose)

## Testing Strategy

### Unit tests
- Argument parsing (clap definitions)
- Exit code mapping
- NDJSON event parsing

### Integration tests
- Use `mediatool-core.exe --selftest` to drive the CLI without real downloads/conversions
- Mock filesystem operations where needed (similar to existing test suite in `tests/`)
- Test each command end-to-end: argument parsing → job creation → event handling → exit code

### Manual testing
- Download a real video, verify progress output
- Cancel mid-download, verify cleanup
- Convert a file, check ffmpeg output parsing
- Batch job with multiple concurrent operations
- Invalid inputs (missing files, bad URLs, unsupported formats)

## Future Enhancements

- Config file support: `~/.gravity/config.json` for default `--output-dir`, `--quality`, etc.
- Preset system: `gravity preset create my-preset --quality 720P --output-dir ~/Videos`
- Watch mode: `gravity watch ~/input_folder` — automatically convert/compress files as they're added
- Server mode: `gravity serve --port 8080` — HTTP API wrapper around the CLI (for headless VMs, containers)
- Shell completions for bash/zsh/PowerShell
