"""Bridges yt-dlp to MediaTool's NDJSON download protocol over stdio.

Protocol: docs/ipc-contract.md, section "Python downloader (downloader.py) <-> C++ core".
Only the standard library plus yt_dlp may be imported here (see requirements.txt) --
the venv this runs in provides nothing else.

Two modes, mutually exclusive:
  --selftest       emits a canned event sequence, no network access, exits 0.
  --command-stdin  reads one {"command":"download","params":{...}} line from stdin,
                    then drives a real yt-dlp download.

Every stdout line is exactly one JSON object (json.dumps + flush). Anything else
(yt-dlp's own log chatter, debug output) goes to stderr instead.
"""

import argparse
import json
import os
import re
import socket
import sys
import traceback
import urllib.error

try:
    import yt_dlp
except ImportError:  # --selftest never touches yt_dlp, so this must not be fatal here
    yt_dlp = None


_ILLEGAL_WINDOWS_CHARS = re.compile(r'[<>:"/\\|?*\x00-\x1f]')


def emit(event: str, data: dict) -> None:
    print(json.dumps({"event": event, "data": data}), flush=True)


def sanitize_filename(name: str) -> str:
    """Best-effort local fallback; core/filesystem/FilenameSanitizer is authoritative."""
    cleaned = _ILLEGAL_WINDOWS_CHARS.sub("_", name or "")
    cleaned = cleaned.rstrip(" .")
    return cleaned or "download"


def format_selector_for_quality(quality: str) -> str:
    if not quality or quality == "best":
        return "bestvideo*+bestaudio/best"
    if quality == "worst":
        return "worstvideo+worstaudio/worst"
    match = re.match(r"^(\d+)p?$", quality)
    if match:
        height = match.group(1)
        return f"bestvideo[height<={height}]+bestaudio/best[height<={height}]"
    return quality  # assume the caller passed a raw yt-dlp format selector


def classify_exception(exc: BaseException):
    """Heuristic mapping to ErrorCategory -- obviously-network failures vs everything else."""
    network_types = (
        socket.timeout,
        socket.gaierror,
        ConnectionError,
        TimeoutError,
        urllib.error.URLError,
    )
    if isinstance(exc, network_types):
        return "E_NETWORK", "NETWORK_ERROR"
    message = str(exc).lower()
    network_keywords = (
        "network", "timed out", "timeout", "connection", "unreachable",
        "unable to download webpage", "name or service not known",
        "getaddrinfo failed", "dns", "temporary failure in name resolution",
    )
    if any(keyword in message for keyword in network_keywords):
        return "E_NETWORK", "NETWORK_ERROR"
    return "E_DOWNLOAD_FAILED", "UNKNOWN"


def emit_error(exc: BaseException) -> None:
    code, category = classify_exception(exc)
    emit("error", {
        "code": code,
        "category": category,
        "message": str(exc) or exc.__class__.__name__,
        "details": traceback.format_exc(),
        "recoverable": category == "NETWORK_ERROR",
    })


class _StderrLogger:
    """Routes yt-dlp's own log chatter to stderr so stdout stays pure NDJSON."""

    def debug(self, msg):
        print(msg, file=sys.stderr)

    def info(self, msg):
        print(msg, file=sys.stderr)

    def warning(self, msg):
        print(msg, file=sys.stderr)

    def error(self, msg):
        print(msg, file=sys.stderr)


def run_selftest() -> int:
    emit("metadata", {
        "title": "MediaTool Self-Test Video",
        "duration": 12,
        "playlistIndex": None,
        "playlistCount": None,
    })

    total_bytes = 4194304
    # Increasing downloadedBytes, decreasing etaSeconds -- what the protocol test asserts.
    steps = [
        (1048576, 9),
        (2097152, 6),
        (3145728, 3),
        (4194304, 0),
    ]
    for downloaded, eta in steps:
        emit("progress", {
            "downloadedBytes": downloaded,
            "totalBytes": total_bytes,
            "speedBytesPerSecond": 1048576,
            "etaSeconds": eta,
            "statusMessage": "Downloading",
        })

    emit("completed", {"outputPath": os.path.join(os.getcwd(), "selftest-output.mp4")})
    return 0


def run_command_stdin() -> int:
    raw_line = sys.stdin.readline()
    try:
        request = json.loads(raw_line)
        command = request.get("command")
        params = request.get("params", {})
        if command != "download":
            raise ValueError(f"unsupported command: {command!r}")
        url = params["url"]
        output_dir = params["outputDir"]
        quality = params.get("quality", "best")
    except Exception as exc:  # malformed request from the C++ core, not a download failure
        emit_error(exc)
        return 1

    if yt_dlp is None:
        emit_error(RuntimeError("yt_dlp is not installed in this environment"))
        return 1

    try:
        probe_opts = {
            "quiet": True,
            "no_warnings": True,
            "noprogress": True,
            "logger": _StderrLogger(),
            "skip_download": True,
        }
        with yt_dlp.YoutubeDL(probe_opts) as probe:
            info = probe.extract_info(url, download=False)

        title = info.get("title") or "download"
        duration = info.get("duration")
        playlist_index = info.get("playlist_index")
        playlist_count = info.get("n_entries") or info.get("playlist_count")
        emit("metadata", {
            "title": title,
            "duration": duration,
            "playlistIndex": playlist_index,
            "playlistCount": playlist_count,
        })

        safe_title = sanitize_filename(title)
        os.makedirs(output_dir, exist_ok=True)
        outtmpl = os.path.join(output_dir, safe_title + ".%(ext)s")

        def progress_hook(status: dict) -> None:
            state = status.get("status")
            if state == "downloading":
                emit("progress", {
                    "downloadedBytes": status.get("downloaded_bytes"),
                    "totalBytes": status.get("total_bytes") or status.get("total_bytes_estimate"),
                    "speedBytesPerSecond": status.get("speed"),
                    "etaSeconds": status.get("eta"),
                    "statusMessage": "Downloading",
                })
            elif state == "finished":
                emit("progress", {
                    "downloadedBytes": status.get("downloaded_bytes") or status.get("total_bytes"),
                    "totalBytes": status.get("total_bytes"),
                    "speedBytesPerSecond": None,
                    "etaSeconds": 0,
                    "statusMessage": "Finalizing",
                })

        download_opts = {
            "outtmpl": outtmpl,
            "format": format_selector_for_quality(quality),
            "progress_hooks": [progress_hook],
            "quiet": True,
            "no_warnings": True,
            "noprogress": True,
            "logger": _StderrLogger(),
        }
        with yt_dlp.YoutubeDL(download_opts) as downloader:
            result_info = downloader.extract_info(url, download=True)
            output_path = downloader.prepare_filename(result_info)

        emit("completed", {"outputPath": output_path})
        return 0
    except Exception as exc:
        emit_error(exc)
        return 1


def main() -> int:
    parser = argparse.ArgumentParser(description="MediaTool yt-dlp downloader bridge")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--selftest", action="store_true",
                       help="Emit a canned NDJSON sequence, no network access")
    mode.add_argument("--command-stdin", action="store_true",
                       help="Read one JSON download command from stdin")
    args = parser.parse_args()

    if args.selftest:
        return run_selftest()
    return run_command_stdin()


if __name__ == "__main__":
    sys.exit(main())
