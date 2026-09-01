"""Protocol-level tests for downloader.py's Phase 2 additions (inspect command, richer
error classification, filename/format-selector plumbing) -- unittest only, no pytest, no
extra deps (see test_downloader_selftest.py's docstring for why). Runs under the ambient
interpreter, which deliberately does NOT have yt_dlp installed -- every test here either
exercises a pure helper function directly or a code path that fails before ever touching
yt_dlp, so that absence is itself part of what a couple of these tests verify.
"""

import importlib.util
import json
import subprocess
import sys
import unittest
from pathlib import Path

DOWNLOADER_SCRIPT = Path(__file__).resolve().parents[2] / "python" / "downloader" / "downloader.py"


def _load_downloader_module():
    spec = importlib.util.spec_from_file_location("downloader", DOWNLOADER_SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


downloader = _load_downloader_module()


def run_command_stdin(payload: str):
    result = subprocess.run(
        [sys.executable, str(DOWNLOADER_SCRIPT), "--command-stdin"],
        input=payload,
        capture_output=True,
        text=True,
        timeout=15,
    )
    lines = [line for line in result.stdout.splitlines() if line.strip()]
    events = [json.loads(line) for line in lines]
    return result, events


class SanitizeFilenameTest(unittest.TestCase):
    def test_strips_illegal_windows_characters(self):
        self.assertEqual(downloader.sanitize_filename('a<b>c:d"e/f\\g|h?i*j'), "a_b_c_d_e_f_g_h_i_j")

    def test_trims_trailing_dots_and_spaces(self):
        self.assertEqual(downloader.sanitize_filename("My Video... "), "My Video")

    def test_falls_back_to_download_when_nothing_survives(self):
        self.assertEqual(downloader.sanitize_filename(""), "download")
        self.assertEqual(downloader.sanitize_filename(None), "download")


class ClassifyExceptionTest(unittest.TestCase):
    def test_downloader_error_uses_its_own_classification_verbatim(self):
        exc = downloader.DownloaderError("E_PLAYLIST_NOT_SUPPORTED", "UNSUPPORTED_FORMAT", "nope")
        code, category, recoverable = downloader.classify_exception(exc)
        self.assertEqual(code, "E_PLAYLIST_NOT_SUPPORTED")
        self.assertEqual(category, "UNSUPPORTED_FORMAT")
        self.assertFalse(recoverable)

    def test_recognizes_private_video(self):
        code, category, _ = downloader.classify_exception(Exception("ERROR: Private video"))
        self.assertEqual(code, "E_VIDEO_PRIVATE")
        self.assertEqual(category, "PERMISSION_ERROR")

    def test_recognizes_removed_video(self):
        code, category, _ = downloader.classify_exception(
            Exception("This video has been removed by the uploader"))
        self.assertEqual(code, "E_VIDEO_REMOVED")
        self.assertEqual(category, "DOWNLOAD_FAILURE")

    def test_recognizes_geo_restriction(self):
        code, category, _ = downloader.classify_exception(
            Exception("The uploader has not made this video available in your country"))
        self.assertEqual(code, "E_GEO_RESTRICTED")
        self.assertEqual(category, "PERMISSION_ERROR")

    def test_recognizes_network_failure_by_type(self):
        import socket
        code, category, recoverable = downloader.classify_exception(socket.timeout("timed out"))
        self.assertEqual(code, "E_NETWORK")
        self.assertEqual(category, "NETWORK_ERROR")
        self.assertTrue(recoverable)

    # --- Merge / post-processing failures -------------------------------------------
    # These fail after the bytes are already on disk, in yt-dlp's own ffmpeg step. They
    # used to fall through to E_DOWNLOAD_FAILED/UNKNOWN, which tells a retry policy
    # nothing -- the whole point of classifying them is the recoverable/permanent split.
    # The message strings below are yt-dlp's own (verified against its source at
    # 2026.8.19; the pattern table cites the exact files and lines).

    def test_missing_merge_tool_is_permanent(self):
        code, category, recoverable = downloader.classify_exception(Exception(
            "You have requested merging of multiple formats but ffmpeg is not installed. "
            "Aborting due to --abort-on-error"))
        self.assertEqual(code, "E_MERGE_TOOL_MISSING")
        self.assertEqual(category, "ENGINE_FAILURE")
        # Retrying cannot install ffmpeg; a retry would only re-download and re-fail.
        self.assertFalse(recoverable)

    def test_postprocessor_missing_ffmpeg_is_permanent(self):
        code, category, recoverable = downloader.classify_exception(Exception(
            "ffmpeg not found. Please install or provide the path using --ffmpeg-location"))
        self.assertEqual(code, "E_MERGE_TOOL_MISSING")
        self.assertEqual(category, "ENGINE_FAILURE")
        self.assertFalse(recoverable)

    def test_postprocessing_failure_is_permanent(self):
        code, category, recoverable = downloader.classify_exception(Exception(
            "Postprocessing: Error opening output files: Invalid argument"))
        self.assertEqual(code, "E_MERGE_FAILED")
        self.assertEqual(category, "ENGINE_FAILURE")
        self.assertFalse(recoverable)

    def test_fragment_transport_failure_is_recoverable(self):
        code, category, recoverable = downloader.classify_exception(Exception(
            "Unable to open fragment 17; HTTP Error 503: Service Unavailable"))
        self.assertEqual(code, "E_FRAGMENT_DOWNLOAD_FAILED")
        self.assertEqual(category, "NETWORK_ERROR")
        # The other half of the split: a fragment that failed to transfer genuinely often
        # succeeds on a second attempt.
        self.assertTrue(recoverable)

    def test_missing_fragment_is_permanent(self):
        code, _, recoverable = downloader.classify_exception(Exception(
            "fragment 4 not found, unable to continue"))
        self.assertEqual(code, "E_FRAGMENT_MISSING")
        self.assertFalse(recoverable)

    def test_a_more_specific_merge_pattern_wins_over_the_postprocessing_prefix(self):
        # yt-dlp prefixes the underlying message with "Postprocessing: ", so both patterns
        # match this string. Ordering in the table is what makes the specific one win --
        # without it every post-processing failure collapses to one opaque code.
        code, _, _ = downloader.classify_exception(Exception(
            "Postprocessing: ffmpeg not found. Please install or provide the path using "
            "--ffmpeg-location"))
        self.assertEqual(code, "E_MERGE_TOOL_MISSING")

    def test_unrecognized_message_falls_back_to_unknown(self):
        code, category, _ = downloader.classify_exception(Exception("completely novel failure text"))
        self.assertEqual(code, "E_DOWNLOAD_FAILED")
        self.assertEqual(category, "UNKNOWN")


class FormatEntryTest(unittest.TestCase):
    def test_video_only_format(self):
        entry = downloader.format_entry({
            "format_id": "137", "ext": "mp4", "width": 1920, "height": 1080, "fps": 30,
            "vcodec": "avc1.640028", "acodec": "none", "vbr": 2500, "filesize": 123456,
        })
        self.assertTrue(entry["hasVideo"])
        self.assertFalse(entry["hasAudio"])
        self.assertEqual(entry["resolution"], "1920x1080")
        self.assertIsNone(entry["audioCodec"])

    def test_audio_only_format(self):
        entry = downloader.format_entry({
            "format_id": "140", "ext": "m4a", "vcodec": "none", "acodec": "mp4a.40.2", "abr": 128,
        })
        self.assertFalse(entry["hasVideo"])
        self.assertTrue(entry["hasAudio"])
        self.assertIsNone(entry["resolution"])
        self.assertIsNone(entry["videoCodec"])


class BuildMetadataPayloadTest(unittest.TestCase):
    def test_single_video_payload(self):
        info = {
            "title": "Test Video", "uploader": "Someone", "duration": 42.0,
            "webpage_url": "https://example.com/watch?v=abc", "thumbnail": "https://example.com/t.jpg",
            "extractor_key": "Generic",
            "formats": [
                {"format_id": "137", "ext": "mp4", "vcodec": "avc1", "acodec": "none"},
                {"format_id": "sb0", "ext": "mhtml", "vcodec": "none", "acodec": "none"},  # storyboard, dropped
            ],
        }
        payload = downloader.build_metadata_payload(info, "https://example.com/watch?v=abc")
        self.assertEqual(payload["title"], "Test Video")
        self.assertEqual(payload["uploader"], "Someone")
        self.assertEqual(len(payload["formats"]), 1)  # storyboard-only format filtered out

    def test_playlist_raises_downloader_error(self):
        with self.assertRaises(downloader.DownloaderError) as ctx:
            downloader.build_metadata_payload({"_type": "playlist"}, "https://example.com/playlist?list=x")
        self.assertEqual(ctx.exception.code, "E_PLAYLIST_NOT_SUPPORTED")
        self.assertEqual(ctx.exception.category, "UNSUPPORTED_FORMAT")


class CommandStdinProtocolTest(unittest.TestCase):
    def test_malformed_json_emits_error_and_exits_nonzero(self):
        result, events = run_command_stdin("not json at all")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]["event"], "error")

    def test_unsupported_command_emits_error(self):
        result, events = run_command_stdin(json.dumps({"command": "conjure", "params": {}}))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(events[0]["event"], "error")
        self.assertIn("unsupported command", events[0]["data"]["message"].lower())

    def test_inspect_missing_url_emits_error(self):
        result, events = run_command_stdin(json.dumps({"command": "inspect", "params": {}}))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(events[0]["event"], "error")

    def test_download_missing_required_keys_emits_error(self):
        result, events = run_command_stdin(json.dumps({"command": "download", "params": {"url": "x"}}))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(events[0]["event"], "error")

    def test_inspect_without_yt_dlp_installed_emits_clean_error(self):
        # This suite's ambient interpreter deliberately has no yt_dlp (see module
        # docstring) -- exercising exactly the "yt_dlp is not installed" guard path.
        if downloader.yt_dlp is not None:
            self.skipTest("ambient interpreter unexpectedly has yt_dlp installed")
        result, events = run_command_stdin(
            json.dumps({"command": "inspect", "params": {"url": "https://example.com/watch?v=x"}}))
        self.assertEqual(result.returncode, 1)
        self.assertEqual(events[-1]["event"], "error")
        self.assertIn("yt_dlp", events[-1]["data"]["message"])


if __name__ == "__main__":
    unittest.main()
