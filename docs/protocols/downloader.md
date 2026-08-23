# Python downloader protocol

The wire contract between `engines/downloader/YtDlpProvider` (C++) and
`python/downloader/downloader.py`. This is a sub-protocol of the overall NDJSON framing
described in `docs/ipc-contract.md` ("Python downloader (downloader.py) <-> C++ core") —
read that section first for the framing rules (one JSON object per line, no length
prefixes) and the process-lifetime model (one process instance == one logical operation,
command on stdin, events streamed on stdout until exit).

`downloader.py --selftest` emits a canned event sequence for the `download` flow with no
network access, so the protocol shape can be verified without hitting a real URL — see
`tests/python/test_downloader_selftest.py`. Everything else described here (`inspect`, the
richer metadata/error shapes) is exercised by `tests/python/test_downloader_protocol.py`
without touching the network either, by testing pure helper functions directly and by
exercising failure paths that never reach `yt_dlp` (malformed input, missing keys, or —
deliberately — an environment where `yt_dlp` isn't installed at all).

## stdout vs. stderr

stdout is reserved *exclusively* for the NDJSON protocol. yt-dlp's own log chatter and any
debug/info/warning output goes to stderr instead (`_StderrLogger` in `downloader.py`). The
C++ core captures both, but only parses stdout as protocol lines — this is what makes it
safe for yt-dlp to be as chatty as it wants without corrupting the wire format.

## Commands (C++ -> Python, one line on stdin)

### `inspect`

Fetches metadata for a URL — including every selectable format, for quality selection —
without downloading anything.

```json
{"command": "inspect", "params": {"url": "https://example.com/watch?v=abc123"}}
```

Emits exactly one `metadata` event (rich shape, see below) followed by one `completed`
event (empty `data`, just marks "this process is done, no error") on success, or one
`error` event on failure. The process then exits.

### `download`

```json
{
  "command": "download",
  "params": {
    "url": "https://example.com/watch?v=abc123",
    "outputDir": "D:\\Videos",
    "formatSelector": "bestvideo[height<=1080]+bestaudio/best[height<=1080]",
    "filenameBase": "My Video Title",
    "ffmpegLocation": "C:\\tools\\ffmpeg\\ffmpeg.exe"
  }
}
```

- `formatSelector` is a **concrete yt-dlp `-f` selector string**, not an application-level
  quality name. `python/downloader/downloader.py` never interprets a quality preset —
  `engines/downloader/YtDlpFormatSelector.h` (C++) is the only place that translation
  happens (spec section 10; see `docs/decisions.md`). This field is required to already be
  in yt-dlp's syntax; downloader.py falls back to `bestvideo*+bestaudio/best` only if it's
  missing entirely (defense in depth, not an expected code path).
- `filenameBase` has **no extension** — yt-dlp appends the real one once the container is
  known (`outtmpl = filenameBase + ".%(ext)s"`). The C++ side is responsible for making
  this collision-free before sending it (`filesystem::DeduplicateBaseName`) — downloader.py
  applies its own local sanitizer as a defense-in-depth pass only, it is not the authority
  (`core/filesystem/FilenameSanitizer` is).
- `ffmpegLocation` is optional. When present, it's passed through as yt-dlp's own
  `ffmpeg_location` option so yt-dlp's internal merge step (triggered whenever
  `formatSelector` picks separate video+audio streams) uses the exact ffmpeg binary the
  C++ core already resolved via `engines/ffmpeg/FFmpegDiscovery`, instead of yt-dlp
  running an independent PATH search. Omitted entirely means "let yt-dlp fall back to its
  own discovery."

Emits one lightweight `metadata` event (title/duration/playlist fields only — the rich
format list was already delivered by an earlier `inspect` call), zero or more `progress`
events, then either one `completed` event (with `outputPath`) or one `error` event.

## Events (Python -> C++, streamed on stdout)

### `metadata`

Lightweight shape (emitted mid-`download`):
```json
{"event": "metadata", "data": {"title": "...", "duration": 123.4, "playlistIndex": null, "playlistCount": null}}
```

Rich shape (emitted by `inspect`) — every field beyond `title` is optional and may be
`null`/absent when the extractor doesn't provide it:
```json
{
  "event": "metadata",
  "data": {
    "title": "My Video Title",
    "uploader": "Some Channel",
    "duration": 723.5,
    "webpageUrl": "https://example.com/watch?v=abc123",
    "thumbnailUrl": "https://example.com/thumb.jpg",
    "extractor": "Youtube",
    "playlistIndex": null,
    "playlistCount": null,
    "formats": [
      {
        "formatId": "137",
        "extension": "mp4",
        "resolution": "1920x1080",
        "width": 1920,
        "height": 1080,
        "fps": 30,
        "videoCodec": "avc1.640028",
        "audioCodec": null,
        "videoBitrateKbps": 2500,
        "audioBitrateKbps": null,
        "filesizeBytes": 104857600,
        "approxFilesizeBytes": null,
        "hasVideo": true,
        "hasAudio": false
      }
    ]
  }
}
```

`formats` only includes entries with at least one of `hasVideo`/`hasAudio` true — yt-dlp's
own non-media "formats" (e.g. storyboard `mhtml` thumbnails-over-time) are filtered out.

### `progress`

Unchanged from Phase 1:
```json
{"event": "progress", "data": {"downloadedBytes": 1048576, "totalBytes": 52428800, "speedBytesPerSecond": 2097152, "etaSeconds": 24, "statusMessage": "Downloading"}}
```

### `completed`

`download`: `{"event": "completed", "data": {"outputPath": "D:\\Videos\\My Video Title.mp4"}}`

`inspect`: `{"event": "completed", "data": {}}` — marks successful process exit; the
result already arrived via the preceding `metadata` event.

### `error`

```json
{"event": "error", "data": {"code": "E_VIDEO_PRIVATE", "category": "PERMISSION_ERROR", "message": "...", "details": "<traceback>", "recoverable": false}}
```

`classify_exception()` in `downloader.py` maps yt-dlp's exception text to one of the codes
below via substring matching (yt-dlp doesn't expose a stable machine-readable error enum,
so this is inherently a heuristic — unmatched failures fall back to
`E_DOWNLOAD_FAILED`/`UNKNOWN`, not a crash):

| code | category | trigger (substring match on the exception message) |
|---|---|---|
| `E_VIDEO_PRIVATE` | `PERMISSION_ERROR` | "private video", "sign in" |
| `E_VIDEO_UNAVAILABLE` | `DOWNLOAD_FAILURE` | "this video is unavailable", "video unavailable" |
| `E_VIDEO_REMOVED` | `DOWNLOAD_FAILURE` | "has been removed", account-terminated wording |
| `E_GEO_RESTRICTED` | `PERMISSION_ERROR` | "not available in your country" |
| `E_FORMAT_UNAVAILABLE` | `UNSUPPORTED_FORMAT` | "requested format is not available", "no video formats found" |
| `E_UNSUPPORTED_URL` | `UNSUPPORTED_FORMAT` | "unsupported url" |
| `E_DISK_SPACE` | `DISK_SPACE_ERROR` | "no space left on device" |
| `E_PERMISSION_DENIED` | `PERMISSION_ERROR` | "permission denied" |
| `E_PLAYLIST_NOT_SUPPORTED` | `UNSUPPORTED_FORMAT` | raised directly (not text-matched) when `inspect`/`download` receives a playlist URL |
| `E_NETWORK` | `NETWORK_ERROR` | a real `socket`/`urllib` exception type, or Phase 1's original network-keyword list |
| `E_DOWNLOAD_FAILED` | `UNKNOWN` | fallback — nothing else matched |

Only `E_NETWORK` is marked `recoverable: true` — every other classification represents a
condition retrying the exact same request won't fix.

## Filename handling — who does what

1. C++ (`DownloadJob::Execute()`) sanitizes the video title
   (`FilenameSanitizer::SanitizeWindowsFilename`) and resolves it against the output
   directory's existing contents (`FilenameSanitizer::DeduplicateBaseName`, matched
   against any extension). This is the authoritative pass (spec section 12: "do not rely
   on the frontend to sanitize filenames" — extended here to also not rely on Python).
2. `downloader.py` applies its own local `sanitize_filename()` to whatever `filenameBase`
   it receives, as a defense-in-depth second pass only — it must never be the only thing
   standing between a raw video title and a Windows path.
3. yt-dlp appends the final extension once it knows the container
   (`outtmpl = filenameBase + ".%(ext)s")`.

## Manual integration test

There is no automated test that downloads a real video (spec section 41 — a live URL in a
mandatory suite is fragile and can expire/change). To manually verify the real pipeline:

```bash
python/downloader/.venv/Scripts/python -c "
import json, subprocess, sys
p = subprocess.run(
    [sys.executable, 'python/downloader/downloader.py', '--command-stdin'],
    input=json.dumps({'command': 'inspect', 'params': {'url': '<a public test URL>'}}),
    capture_output=True, text=True)
print(p.stdout)
print('---stderr---', file=sys.stderr)
print(p.stderr, file=sys.stderr)
"
```

or, for the full app, follow the Phase 2 acceptance test in `docs/phase-2.md`.
