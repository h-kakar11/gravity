#!/usr/bin/env python3
"""A stand-in for python/downloader/downloader.py that speaks the real protocol.

Exists so the DOWNLOAD path -- and specifically automatic retry of a transient
failure -- can be exercised end to end against the real mediatool-core binary without
depending on YouTube being up, or on a real network at all (spec section 50).

It is a real child process, launched by the real YtDlpProvider over the real NDJSON
stdio protocol in docs/protocols/downloader.md. Only yt-dlp itself is replaced.

Behaviour is driven by env vars:
  FAKE_DL_STATE        file used to count attempts across process launches
  FAKE_DL_FAIL_TIMES   how many download attempts fail before one succeeds
  FAKE_DL_FAIL_CODE    error code to emit  (default E_DOWNLOAD_TRANSPORT_ERROR)
  FAKE_DL_FAIL_CATEGORY error category      (default NETWORK_ERROR)
"""
import json
import os
import sys


def emit(event, data):
    sys.stdout.write(json.dumps({"event": event, "data": data}) + "\n")
    sys.stdout.flush()


def bump_attempts():
    path = os.environ.get("FAKE_DL_STATE", "")
    if not path:
        return 1
    count = 0
    if os.path.exists(path):
        with open(path) as f:
            count = int(f.read().strip() or "0")
    count += 1
    with open(path, "w") as f:
        f.write(str(count))
    return count


def main():
    if "--selftest" in sys.argv:
        emit("metadata", {"title": "selftest", "duration": 1.0})
        emit("completed", {})
        return 0

    line = sys.stdin.readline()
    if not line.strip():
        return 0
    request = json.loads(line)
    command = request.get("command")
    params = request.get("params", {})

    if command == "inspect":
        emit("metadata", {
            "title": "Fake Clip",
            "uploader": "Fake Channel",
            "duration": 2.0,
            "webpageUrl": params.get("url"),
            "extractor": "Fake",
            "formats": [{
                "formatId": "18", "extension": "mp4", "resolution": "320x240",
                "width": 320, "height": 240, "hasVideo": True, "hasAudio": True,
            }],
        })
        emit("completed", {})
        return 0

    if command == "download":
        attempt = bump_attempts()
        fail_times = int(os.environ.get("FAKE_DL_FAIL_TIMES", "0"))
        if attempt <= fail_times:
            emit("error", {
                "code": os.environ.get("FAKE_DL_FAIL_CODE", "E_DOWNLOAD_TRANSPORT_ERROR"),
                "category": os.environ.get("FAKE_DL_FAIL_CATEGORY", "NETWORK_ERROR"),
                "message": "The connection dropped partway through.",
                "details": f"simulated failure on attempt {attempt}",
                "recoverable": True,
            })
            return 1

        emit("metadata", {"title": "Fake Clip", "duration": 2.0,
                          "playlistIndex": None, "playlistCount": None})
        out_dir = params["outputDir"]
        os.makedirs(out_dir, exist_ok=True)
        output = os.path.join(out_dir, params["filenameBase"] + ".mp4")

        if os.environ.get("FAKE_DL_HANG"):
            # Simulate a real in-progress download: yt-dlp's own partial artifact on
            # disk, then hang until the parent terminates/kills us -- exactly what a
            # real cancellation mid-download looks like (spec section 27/50).
            partial = os.path.join(out_dir, params["filenameBase"] + ".mp4.part")
            with open(partial, "wb") as f:
                f.write(b"partial bytes, not a real container")
            emit("progress", {"percentage": 10.0, "processedBytes": 1000,
                              "totalBytes": 10000, "speedBytesPerSecond": 5000.0,
                              "etaSeconds": 5.0, "statusMessage": "Downloading"})
            import time
            time.sleep(300)
            return 0

        # A real, probeable file -- DownloadJob verifies its output with ffprobe, so
        # writing junk bytes here would fail verification and prove nothing.
        os.system(
            "ffmpeg -hide_banner -loglevel error -y -f lavfi "
            "-i testsrc=size=320x240:rate=10:duration=2 -c:v libx264 "
            f"'{output}' >/dev/null 2>&1")
        for pct in (25.0, 60.0, 100.0):
            emit("progress", {"percentage": pct, "processedBytes": int(pct * 100),
                              "totalBytes": 10000, "speedBytesPerSecond": 5000.0,
                              "etaSeconds": 1.0, "statusMessage": "Downloading"})
        emit("completed", {"outputPath": output})
        return 0

    emit("error", {"code": "E_UNKNOWN_COMMAND", "category": "UNKNOWN",
                   "message": f"unknown command {command}", "details": "",
                   "recoverable": False})
    return 1


if __name__ == "__main__":
    sys.exit(main())
