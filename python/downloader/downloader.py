"""Bridges yt-dlp to MediaTool's NDJSON download protocol over stdio.

Protocol: docs/protocols/downloader.md (summarized in docs/ipc-contract.md). Only the
standard library plus yt_dlp may be imported here (see requirements.txt) -- the venv this
runs in provides nothing else.

Three modes, mutually exclusive:
  --selftest       emits a canned event sequence, no network access, exits 0.
  --command-stdin  reads one JSON command line from stdin, then acts on it:
                      {"command": "inspect", "params": {"url": ...}}
                      {"command": "download", "params": {"url", "outputDir",
                                                          "formatSelector", "filenameBase",
                                                          "ffmpegLocation"?}}

Every stdout line is exactly one JSON object (json.dumps + flush). Anything else
(yt-dlp's own log chatter, debug output) goes to stderr instead -- stdout is reserved for
the NDJSON protocol so the C++ core never has to distinguish protocol lines from noise.

Format selection and quality presets are NOT resolved here: engines/downloader (C++) is
the only place that translates an application-level QualityPreset into a concrete yt-dlp
selector string (spec section 10) -- this script just executes whatever `formatSelector`
it's given.
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


class DownloaderError(Exception):
    """Base for errors this script classifies itself, bypassing the exit-message
    heuristics in classify_exception() -- raised with a code/category already decided."""

    def __init__(self, code: str, category: str, message: str, recoverable: bool = False):
        super().__init__(message)
        self.code = code
        self.category = category
        self.recoverable = recoverable


def emit(event: str, data: dict) -> None:
    print(json.dumps({"event": event, "data": data}), flush=True)


def sanitize_filename(name: str) -> str:
    """Best-effort local fallback; core/filesystem/FilenameSanitizer is authoritative."""
    cleaned = _ILLEGAL_WINDOWS_CHARS.sub("_", name or "")
    cleaned = cleaned.rstrip(" .")
    return cleaned or "download"


# (message substring, code, category, recoverable) -- checked in order, first match wins.
# This is a heuristic over yt-dlp's exception text (it doesn't expose a stable error-code
# enum), not a guarantee every phrasing yt-dlp ever uses is covered -- unmatched failures
# fall through to the generic E_DOWNLOAD_FAILED/UNKNOWN classification below.
_KNOWN_FAILURE_PATTERNS = (
    ("private video", "E_VIDEO_PRIVATE", "PERMISSION_ERROR", False),
    ("sign in", "E_VIDEO_PRIVATE", "PERMISSION_ERROR", False),
    ("this video is unavailable", "E_VIDEO_UNAVAILABLE", "DOWNLOAD_FAILURE", False),
    ("video unavailable", "E_VIDEO_UNAVAILABLE", "DOWNLOAD_FAILURE", False),
    ("has been removed", "E_VIDEO_REMOVED", "DOWNLOAD_FAILURE", False),
    ("account associated with this video has been terminated", "E_VIDEO_REMOVED", "DOWNLOAD_FAILURE", False),
    ("not available in your country", "E_GEO_RESTRICTED", "PERMISSION_ERROR", False),
    ("not made this video available in your country", "E_GEO_RESTRICTED", "PERMISSION_ERROR", False),
    ("requested format is not available", "E_FORMAT_UNAVAILABLE", "UNSUPPORTED_FORMAT", False),
    ("no video formats found", "E_FORMAT_UNAVAILABLE", "UNSUPPORTED_FORMAT", False),
    ("unsupported url", "E_UNSUPPORTED_URL", "UNSUPPORTED_FORMAT", False),
    ("no space left on device", "E_DISK_SPACE", "DISK_SPACE_ERROR", False),
    ("permission denied", "E_PERMISSION_DENIED", "PERMISSION_ERROR", False),

    # --- Merge / post-processing -------------------------------------------------------
    # Everything above fails before a single byte is written. These fail AFTER the bytes
    # are on disk, in yt-dlp's own ffmpeg step, and they split cleanly into two kinds --
    # which is the whole reason they are classified separately instead of falling through
    # to E_DOWNLOAD_FAILED/UNKNOWN, where a retry policy has nothing to go on.
    #
    #   Permanent: the merge tool is missing, or ffmpeg rejected the streams. Running the
    #   exact same command again produces the exact same failure; the user has to install
    #   ffmpeg or pick a different format. Retrying only wastes the download.
    #
    #   Recoverable: a fragment could not be fetched. That is a transport failure wearing
    #   a download-stage costume, and a second attempt genuinely often works.
    #
    # Substrings verified against yt-dlp's own source (checked at 2026.8.19):
    # YoutubeDL.py "You have requested merging of multiple formats but ffmpeg is not
    # installed" and f"Postprocessing: {err}"; postprocessor/ffmpeg.py "ffmpeg not found.
    # Please install...", "ffprobe and ffmpeg not found...", "ffprobe not found...",
    # f"audio conversion failed: {err.msg}"; downloader/external.py f"Unable to open
    # fragment {frag_index}; {err}"; downloader/fragment.py f"fragment {frag_index} not
    # found, unable to continue".
    ("but ffmpeg is not installed", "E_MERGE_TOOL_MISSING", "ENGINE_FAILURE", False),
    ("ffmpeg not found. please install", "E_MERGE_TOOL_MISSING", "ENGINE_FAILURE", False),
    ("ffprobe not found. please install", "E_MERGE_TOOL_MISSING", "ENGINE_FAILURE", False),
    ("unable to open fragment", "E_FRAGMENT_DOWNLOAD_FAILED", "NETWORK_ERROR", True),
    ("not found, unable to continue", "E_FRAGMENT_MISSING", "DOWNLOAD_FAILURE", False),
    ("audio conversion failed", "E_MERGE_FAILED", "ENGINE_FAILURE", False),
    # Last of the merge group on purpose: yt-dlp prefixes the *underlying* message with
    # "Postprocessing: ", so a more specific pattern above (a missing ffmpeg, say) must
    # get the first look at the same string.
    ("postprocessing:", "E_MERGE_FAILED", "ENGINE_FAILURE", False),
)

_NETWORK_TYPES = (socket.timeout, socket.gaierror, ConnectionError, TimeoutError, urllib.error.URLError)

_NETWORK_KEYWORDS = (
    "network", "timed out", "timeout", "connection", "unreachable",
    "unable to download webpage", "name or service not known",
    "getaddrinfo failed", "dns", "temporary failure in name resolution",
)


def classify_exception(exc: BaseException):
    """Heuristic mapping to (code, category, recoverable) -- see _KNOWN_FAILURE_PATTERNS
    and spec section 25's list of failure kinds a downloader must distinguish."""
    if isinstance(exc, DownloaderError):
        return exc.code, exc.category, exc.recoverable

    if isinstance(exc, _NETWORK_TYPES):
        return "E_NETWORK", "NETWORK_ERROR", True

    message = str(exc).lower()
    for substring, code, category, recoverable in _KNOWN_FAILURE_PATTERNS:
        if substring in message:
            return code, category, recoverable
    if any(keyword in message for keyword in _NETWORK_KEYWORDS):
        return "E_NETWORK", "NETWORK_ERROR", True

    return "E_DOWNLOAD_FAILED", "UNKNOWN", False


def emit_error(exc: BaseException) -> None:
    code, category, recoverable = classify_exception(exc)
    emit("error", {
        "code": code,
        "category": category,
        "message": str(exc) or exc.__class__.__name__,
        "details": traceback.format_exc(),
        "recoverable": recoverable,
    })


class _StderrLogger:
    """Routes yt-dlp's own log chatter to stderr so stdout stays pure NDJSON.

    Every method swallows its own write failure instead of letting it propagate: a video
    title can contain characters a Windows console/pipe rejects outright with a raw
    OSError (not the more common UnicodeEncodeError) rather than transliterating or
    replacing them, and a logging call failing must never take down the whole job with it
    -- see issue #56 (a second retry attempt failed with a traceback originating from this
    exact print() call).
    """

    def _safe_print(self, msg):
        try:
            print(msg, file=sys.stderr)
        except (OSError, UnicodeError):
            try:
                encoding = sys.stderr.encoding or "utf-8"
                sys.stderr.buffer.write(str(msg).encode(encoding, errors="replace") + b"\n")
                sys.stderr.buffer.flush()
            except Exception:
                pass  # best-effort diagnostic logging; never let it fail the job

    def debug(self, msg):
        self._safe_print(msg)

    def info(self, msg):
        self._safe_print(msg)

    def warning(self, msg):
        self._safe_print(msg)

    def error(self, msg):
        self._safe_print(msg)


def format_entry(f: dict) -> dict:
    vcodec = f.get("vcodec")
    acodec = f.get("acodec")
    has_video = bool(vcodec) and vcodec != "none"
    has_audio = bool(acodec) and acodec != "none"
    width = f.get("width")
    height = f.get("height")
    resolution = f"{width}x{height}" if (has_video and width and height) else None
    return {
        "formatId": f.get("format_id") or "",
        "extension": f.get("ext"),
        "resolution": resolution,
        "width": width if has_video else None,
        "height": height if has_video else None,
        "fps": f.get("fps") if has_video else None,
        "videoCodec": vcodec if has_video else None,
        "audioCodec": acodec if has_audio else None,
        "videoBitrateKbps": f.get("vbr") or (f.get("tbr") if has_video else None),
        "audioBitrateKbps": f.get("abr"),
        "filesizeBytes": f.get("filesize"),
        "approxFilesizeBytes": f.get("filesize_approx"),
        "hasVideo": has_video,
        "hasAudio": has_audio,
    }


def build_metadata_payload(info: dict, url: str) -> dict:
    if info.get("_type") == "playlist":
        raise DownloaderError(
            "E_PLAYLIST_NOT_SUPPORTED", "UNSUPPORTED_FORMAT",
            "This URL is a playlist. MediaTool currently supports single-video URLs only.")

    formats = []
    for f in info.get("formats") or []:
        entry = format_entry(f)
        if entry["hasVideo"] or entry["hasAudio"]:
            formats.append(entry)

    return {
        "title": info.get("title") or "Untitled",
        "uploader": info.get("uploader") or info.get("channel"),
        "duration": info.get("duration"),
        "webpageUrl": info.get("webpage_url") or url,
        "thumbnailUrl": info.get("thumbnail"),
        "extractor": info.get("extractor_key") or info.get("extractor"),
        "playlistIndex": info.get("playlist_index"),
        "playlistCount": info.get("n_entries") or info.get("playlist_count"),
        "formats": formats,
    }


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


# Phase 2 supports single videos only (spec section 30) -- these two options make that
# fast rather than merely correct-eventually:
#   noplaylist    -- when a URL points to a video that's ALSO part of a playlist (e.g.
#                     "watch?v=X&list=Y", a very common way people share/copy links),
#                     process just that one video instead of the whole playlist.
#   extract_flat  -- when a URL points to nothing BUT a playlist, don't walk every entry
#                     resolving its full metadata (which can mean dozens of extra network
#                     requests) just to discover it's a playlist -- "in_playlist" keeps
#                     entries shallow while leaving a direct single-video URL unaffected,
#                     so build_metadata_payload()'s `_type == "playlist"` check still
#                     fires, just without the network cost of a full walk first.
_SINGLE_VIDEO_PROBE_OPTS = {
    "quiet": True,
    "no_warnings": True,
    "noprogress": True,
    "logger": _StderrLogger(),
    "skip_download": True,
    "noplaylist": True,
    "extract_flat": "in_playlist",
    # Bounds how long a single stalled network call can block -- without this, a
    # unresponsive host leaves inspect() hanging indefinitely, and since it runs
    # synchronously on the C++ core's single IPC thread (issue #8), that freezes the whole
    # backend, not just this request. This is a partial mitigation (retries still add up,
    # and the C++ side has no independent deadline of its own yet) rather than the full
    # fix issue #8 describes.
    "socket_timeout": 15,
}


def run_inspect(url: str) -> int:
    if yt_dlp is None:
        emit_error(RuntimeError("yt_dlp is not installed in this environment"))
        return 1
    try:
        with yt_dlp.YoutubeDL(dict(_SINGLE_VIDEO_PROBE_OPTS)) as probe:
            info = probe.extract_info(url, download=False)
        emit("metadata", build_metadata_payload(info, url))
        emit("completed", {})
        return 0
    except Exception as exc:
        emit_error(exc)
        return 1


def _resolve_output_path(downloader: "yt_dlp.YoutubeDL", result_info: dict) -> str:
    """`prepare_filename()` is a template renderer, not a report of what yt-dlp actually
    wrote to disk -- it doesn't reflect post-processing (e.g. yt-dlp forcing a different
    container on an incompatible video+audio merge, the common case for anything but the
    lowest-quality formats). The real path is on `requested_downloads[0]['filepath']` once
    download/merge has actually happened; fall back to `prepare_filename()` only if that's
    somehow absent. See issue #25 -- a wrong path here used to trigger the destructive
    cleanup issue #3 fixed, and independently explains issue #56's "completed job reports
    the wrong/intermediate file" symptom.
    """
    requested = result_info.get("requested_downloads") or []
    if requested and requested[0].get("filepath"):
        return requested[0]["filepath"]
    return downloader.prepare_filename(result_info)


def run_download(params: dict) -> int:
    url = params["url"]
    output_dir = params["outputDir"]
    format_selector = params.get("formatSelector") or "bestvideo*+bestaudio/best"
    filename_base = sanitize_filename(params.get("filenameBase") or "download")
    ffmpeg_location = params.get("ffmpegLocation")

    if yt_dlp is None:
        emit_error(RuntimeError("yt_dlp is not installed in this environment"))
        return 1

    try:
        # No separate metadata probe here (issue #35): this used to run its own
        # extract_info(download=False) -- a full extra network round-trip to yt-dlp's
        # extractor on every single download -- purely to emit a "metadata" event that
        # DownloadJob::Execute() (the only real caller of this command) always discards
        # with a no-op onMetadata callback, since it already fetched metadata itself via
        # its own mandatory Inspect() call moments earlier (that's also what rejects a
        # playlist URL before a "download" command is ever sent -- see
        # DownloadJob.PlaylistUrlSurfacesAsUnsupportedFormat -- so `noplaylist` below on
        # the real download is enough; there is no path that reaches this function without
        # already having passed that gate).
        os.makedirs(output_dir, exist_ok=True)
        outtmpl = os.path.join(output_dir, filename_base + ".%(ext)s")

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
            "format": format_selector,
            "progress_hooks": [progress_hook],
            "quiet": True,
            "no_warnings": True,
            "noprogress": True,
            "logger": _StderrLogger(),
            # Defense in depth: DownloadJob always rejects a playlist URL via an earlier
            # Inspect() call before ever reaching here, but a video+playlist combo URL
            # should still resolve to just the one video if this is ever invoked directly.
            "noplaylist": True,
            # Per-socket-operation bound, not a total-download deadline -- only fires if a
            # single connect/read stalls, so it doesn't cut off a legitimately slow but
            # progressing large download. See issue #8.
            "socket_timeout": 15,
        }
        # Points yt-dlp's own internal merge step (when the selector picks separate
        # video+audio streams) at the SAME ffmpeg the C++ core already resolved, instead
        # of letting yt-dlp run a second, independent PATH search -- see
        # docs/decisions.md "Video/audio merge strategy".
        if ffmpeg_location:
            download_opts["ffmpeg_location"] = ffmpeg_location

        with yt_dlp.YoutubeDL(download_opts) as downloader:
            result_info = downloader.extract_info(url, download=True)
            output_path = _resolve_output_path(downloader, result_info)

        emit("completed", {"outputPath": output_path})
        return 0
    except Exception as exc:
        emit_error(exc)
        return 1


def run_command_stdin() -> int:
    raw_line = sys.stdin.readline()
    try:
        request = json.loads(raw_line)
        command = request.get("command")
        params = request.get("params", {})
    except Exception as exc:  # malformed request from the C++ core, not a download failure
        emit_error(exc)
        return 1

    if command == "inspect":
        try:
            url = params["url"]
        except Exception as exc:
            emit_error(exc)
            return 1
        return run_inspect(url)

    if command == "download":
        try:
            _ = params["url"]
            _ = params["outputDir"]
        except Exception as exc:
            emit_error(exc)
            return 1
        return run_download(params)

    emit_error(ValueError(f"unsupported command: {command!r}"))
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description="MediaTool yt-dlp downloader bridge")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--selftest", action="store_true",
                       help="Emit a canned NDJSON sequence, no network access")
    mode.add_argument("--command-stdin", action="store_true",
                       help="Read one JSON command from stdin (inspect or download)")
    args = parser.parse_args()

    if args.selftest:
        return run_selftest()
    return run_command_stdin()


if __name__ == "__main__":
    sys.exit(main())
