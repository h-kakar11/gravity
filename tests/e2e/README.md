# End-to-end queue verification

Two harnesses that drive the **real** `mediatool-core` binary over its **real** NDJSON stdio
protocol. Nothing in the C++ path is mocked: real child processes, real FFmpeg and FFprobe,
real files on disk, real queue state written to and read back from a real state file.

They exist because unit tests cannot demonstrate the things this phase is actually about.
A test with a mock process runner will happily agree that concurrency is capped at two; only
counting real `ffmpeg` children proves it. Both of the bugs that mattered most in Phase 5 --
concurrent jobs overwriting each other's output, and event sequence numbers reaching the
wire out of order -- were found here and were invisible to the unit suite.

## Running them

Build the core first, then from the repository root:

```bash
python3 tests/e2e/queue_ffmpeg_e2e.py     # 75 checks
python3 tests/e2e/queue_download_e2e.py   # 34 checks
```

Requirements: `ffmpeg` and `ffprobe` on `PATH`, Python 3.8+. **No network access is needed
and none is used.**

## `queue_ffmpeg_e2e.py`

Real conversions and compressions, driven end to end. Covers: the queue spanning all three
job types; job metadata; a convert -> compress dependency chain; a failed dependency
skipping its dependents; duplicate detection; the concurrency limit measured against real
`ffmpeg` process counts rather than job objects; cancelling a running encode and confirming
the child was killed and left no partial file; cancelling a queued job without ever starting
one; priority and reordering; fifteen malformed-input rejections; event coherence and
sequence ordering; clearing history without touching files; restart recovery; and starting
up against a deliberately corrupted state file.

## `queue_download_e2e.py`

The DOWNLOAD path, including automatic retry. Covers: real download jobs queueing and
completing; three concurrent same-title downloads producing three distinct files; a transient
failure being retried automatically with real backoff; retries stopping at the budget; a
permanent failure not being retried at all; manual retry of a failed job; and a full
download -> convert -> compress pipeline where each stage's input is resolved by the backend
from the previous stage's actual output.

### Why yt-dlp is stood in for

`fake_downloader.py` speaks the exact protocol in `docs/protocols/downloader.md` and is
launched by the real `YtDlpProvider` as a real child process. Only yt-dlp itself is replaced.

That keeps this suite reproducible and offline, which spec section 50 asks for: a retry test
needs a failure that happens *on demand, twice, then stops*, and no real video hosting
service will oblige. Depending on YouTube would also make the suite fail for reasons that
have nothing to do with this code.

What is therefore **not** covered here, and needs manual verification against a real URL:
yt-dlp's own format selection, extractor behaviour, and its video/audio merge step. Those
are Phase 2 concerns and unchanged by Phase 5; see `docs/protocols/downloader.md`.
