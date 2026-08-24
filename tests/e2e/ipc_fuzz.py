#!/usr/bin/env python3
"""Phase 8 adversarial IPC testing against the REAL mediatool-core binary.

Sends malformed, hostile, and structurally invalid input directly at the real NDJSON
stdin/stdout protocol (docs/ipc-contract.md) that a real Tauri frontend would otherwise
send well-formed. The two properties every case checks:

  1. The process is still alive afterward (no crash).
  2. The NDJSON stream is still coherent afterward -- a known-good `listJobs` call sent
     right after gets back a well-formed, correctly-id'd response. A corrupted stream
     (a stray non-JSON line, a response glued onto another) would make this fail even
     if the process itself didn't crash.

Nothing here should ever need real ffmpeg, yt-dlp, or network access -- this only
exercises the IPC boundary and the input-validation layer in front of it (spec section 54).
"""
import json
import os
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CORE = os.path.abspath("build/linux-debug/app/core/mediatool-core")
WORK = "/tmp/gravity-ipc-fuzz"
PASS, FAIL = [], []


def check(name, condition, detail=""):
    (PASS if condition else FAIL).append(name)
    status = "PASS" if condition else "FAIL"
    print(f"  [{status}] {name}{(' -- ' + detail) if detail else ''}")
    return condition


class Core:
    """Talks to the real binary over raw stdin/stdout, tolerating (and detecting) a
    malformed response rather than assuming every line is valid JSON like the other E2E
    harnesses do -- that assumption is exactly what this file needs to NOT make."""

    def __init__(self):
        os.makedirs(WORK, exist_ok=True)
        env = dict(os.environ)
        env["LOCALAPPDATA"] = WORK
        env["MEDIATOOL_PYTHON_PATH"] = sys.executable
        env["MEDIATOOL_DOWNLOADER_SCRIPT"] = os.path.join(HERE, "fake_downloader.py")
        self.proc = subprocess.Popen(
            [CORE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, bufsize=1, env=env)
        self.responses = {}
        self.malformed_lines = []
        self.lock = threading.Lock()
        self.next_id = 0
        self._stop = False
        threading.Thread(target=self._read_stdout, daemon=True).start()
        threading.Thread(target=self._read_stderr, daemon=True).start()

    def _read_stdout(self):
        for raw in self.proc.stdout:
            if self._stop:
                return
            try:
                line = raw.decode("utf-8", errors="replace")
            except Exception:
                line = repr(raw)
            stripped = line.strip()
            if not stripped:
                continue
            try:
                msg = json.loads(stripped)
            except Exception as e:
                with self.lock:
                    self.malformed_lines.append((stripped, str(e)))
                continue
            with self.lock:
                if isinstance(msg, dict) and "id" in msg:
                    self.responses[msg["id"]] = msg
                # events (no "id") are not needed by this harness

    def _read_stderr(self):
        for _ in self.proc.stderr:
            pass  # drained so the child never blocks on a full stderr pipe

    def send_raw(self, raw_bytes: bytes):
        """Writes exactly these bytes (plus a trailing newline if not already present)
        directly to stdin -- used for input that isn't valid JSON at all, or that a
        json.dumps() call could never produce (duplicate keys, truncated payloads)."""
        try:
            self.proc.stdin.write(raw_bytes)
            if not raw_bytes.endswith(b"\n"):
                self.proc.stdin.write(b"\n")
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError):
            pass  # the process may have exited; alive_after() reports that

    def call(self, command, params=None, req_id=None, timeout=5):
        self.next_id += 1
        rid = req_id if req_id is not None else str(self.next_id)
        request = {"id": rid, "command": command, "params": params if params is not None else {}}
        self.send_raw(json.dumps(request).encode("utf-8"))
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                if rid in self.responses:
                    return self.responses.pop(rid)
            time.sleep(0.02)
        return None

    def alive(self) -> bool:
        return self.proc.poll() is None

    def stream_is_coherent(self) -> bool:
        """The real proof a fuzz input didn't corrupt the protocol: a fresh, ordinary
        request still gets an ordinary, correctly-id'd response, and nothing malformed
        has landed on stdout."""
        probe_id = f"coherence-probe-{self.next_id + 1}"
        response = self.call("listJobs", {}, req_id=probe_id, timeout=5)
        with self.lock:
            corrupted = list(self.malformed_lines)
        ok = response is not None and response.get("id") == probe_id and "ok" in response
        return ok and not corrupted

    def shutdown(self):
        self._stop = True
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()
            self.proc.wait(timeout=5)


def fuzz_case(core: Core, name: str, raw: bytes):
    """Sends one hostile line and verifies the process survived it with the protocol
    stream still intact. This is the workhorse every malformed-input case below calls."""
    core.send_raw(raw)
    alive = core.alive()
    coherent = core.stream_is_coherent() if alive else False
    check(name, alive and coherent, "" if alive else "process died")


def main():
    core = Core()
    time.sleep(0.3)
    check("core process started", core.alive())

    print("\n=== 1. Malformed JSON / framing ===")
    fuzz_case(core, "completely non-JSON garbage", b"not json at all {{{")
    fuzz_case(core, "truncated JSON object", b'{"id": "x", "command": "list')
    fuzz_case(core, "empty object", b"{}")
    fuzz_case(core, "bare JSON null", b"null")
    fuzz_case(core, "a JSON array instead of an object", b'["id", "command"]')
    fuzz_case(core, "a bare JSON number", b"42")
    fuzz_case(core, "an extremely long garbage line (1MB)", b"x" * (1024 * 1024))
    fuzz_case(core, "embedded NUL byte", b'{"id": "x", "command": "listJobs", "params": {}}\x00trailing')
    fuzz_case(core, "invalid UTF-8 bytes", b'{"id": "x", "command": "listJobs", "params": {}}\xff\xfe')
    fuzz_case(
        core, "duplicated JSON keys (last wins per JSON semantics, not a crash)",
        b'{"id": "dup1", "id": "dup2", "command": "listJobs", "params": {}}',
    )

    print("\n=== 2. Missing / wrong-typed required fields ===")
    fuzz_case(core, "missing id", b'{"command": "listJobs", "params": {}}')
    fuzz_case(core, "missing command", b'{"id": "1", "params": {}}')
    fuzz_case(core, "missing params (should default, not crash)", b'{"id": "1", "command": "listJobs"}')
    fuzz_case(core, "id is a number, not a string", b'{"id": 12345, "command": "listJobs", "params": {}}')
    fuzz_case(core, "id is null", b'{"id": null, "command": "listJobs", "params": {}}')
    fuzz_case(core, "command is a number", b'{"id": "1", "command": 42, "params": {}}')
    fuzz_case(core, "command is null", b'{"id": "1", "command": null, "params": {}}')
    fuzz_case(core, "params is a string, not an object", b'{"id": "1", "command": "createJob", "params": "hello"}')
    fuzz_case(core, "params is an array", b'{"id": "1", "command": "createJob", "params": []}')
    fuzz_case(core, "params is a number", b'{"id": "1", "command": "createJob", "params": 7}')
    fuzz_case(core, "params is null", b'{"id": "1", "command": "createJob", "params": null}')

    print("\n=== 3. Unknown / unexpected commands ===")
    check("unknown command returns a clean error, not a crash", (lambda r: r is not None and r.get("ok") is False)(
        core.call("totallyMadeUpCommand", {})))
    fuzz_case(core, "empty command name", b'{"id": "1", "command": "", "params": {}}')
    fuzz_case(core, "command with path-traversal-looking name", b'{"id": "1", "command": "../../etc/passwd", "params": {}}')
    fuzz_case(core, "command that looks like a shell command", b'{"id": "1", "command": "rm -rf /", "params": {}}')

    print("\n=== 4. createJob: invalid enums, missing fields, huge values ===")
    def create_job_bad(name, params):
        r = core.call("createJob", params)
        ok = check(name, r is not None and r.get("ok") is False, json.dumps(r)[:200] if r else "no response")
        return ok

    create_job_bad("unknown job type", {"type": "SOMETHING_MADE_UP", "params": {}})
    create_job_bad("missing type", {"params": {}})
    create_job_bad("type is not a string", {"type": 5, "params": {}})
    create_job_bad("DOWNLOAD missing url", {"type": "DOWNLOAD", "params": {"outputDirectory": WORK}})
    create_job_bad("DOWNLOAD empty url", {"type": "DOWNLOAD", "params": {"url": "", "outputDirectory": WORK}})
    create_job_bad("DOWNLOAD invalid quality enum", {
        "type": "DOWNLOAD", "params": {"url": "https://example.com/x", "outputDirectory": WORK, "quality": "SUPER_ULTRA_HD"}})
    create_job_bad("DOWNLOAD path traversal in outputDirectory", {
        "type": "DOWNLOAD", "params": {"url": "https://example.com/x", "outputDirectory": "../../../../etc"}})
    create_job_bad("DOWNLOAD NUL byte in outputDirectory", {
        "type": "DOWNLOAD", "params": {"url": "https://example.com/x", "outputDirectory": WORK + "\x00/evil"}})
    create_job_bad("DOWNLOAD 1MB url string", {
        "type": "DOWNLOAD", "params": {"url": "https://example.com/" + ("a" * (1024 * 1024)), "outputDirectory": WORK}})
    create_job_bad("negative priority-adjacent retryPolicy.maxRetries", {
        "type": "DOWNLOAD",
        "params": {"url": "https://example.com/x", "outputDirectory": WORK},
        "retryPolicy": {"maxRetries": -5},
    })
    create_job_bad("absurd retryPolicy.maxRetries", {
        "type": "DOWNLOAD",
        "params": {"url": "https://example.com/x", "outputDirectory": WORK},
        "retryPolicy": {"maxRetries": 999999999},
    })
    create_job_bad("dependsOn references an unknown job id (dependency injection of a phantom job)", {
        "type": "CONVERSION",
        "params": {"inputPath": WORK, "outputDirectory": WORK, "targetFormat": "MP4"},
        "dependsOn": ["job-does-not-exist-and-never-will"],
    })
    create_job_bad("dependsOn is not an array", {
        "type": "CONVERSION",
        "params": {"inputPath": WORK, "outputDirectory": WORK, "targetFormat": "MP4"},
        "dependsOn": "job-1",
    })
    create_job_bad("dependsOn has 1000 entries (bound check)", {
        "type": "CONVERSION",
        "params": {"inputPath": WORK, "outputDirectory": WORK, "targetFormat": "MP4"},
        "dependsOn": [f"job-{i}" for i in range(1000)],
    })
    create_job_bad("invalid priority enum", {
        "type": "DOWNLOAD", "params": {"url": "https://example.com/x", "outputDirectory": WORK}, "priority": "MEGA_URGENT"})
    create_job_bad("CONVERSION both inputPath and inputFromJobId set", {
        "type": "CONVERSION",
        "params": {"inputPath": WORK, "inputFromJobId": "job-1", "outputDirectory": WORK, "targetFormat": "MP4"},
    })
    create_job_bad("CONVERSION neither inputPath nor inputFromJobId set", {
        "type": "CONVERSION", "params": {"outputDirectory": WORK, "targetFormat": "MP4"}})
    create_job_bad("CONVERSION unsupported target format", {
        "type": "CONVERSION", "params": {"inputPath": WORK, "outputDirectory": WORK, "targetFormat": "EXOTIC_CODEC"}})

    print("\n=== 5. Job-id-addressed commands: unknown/malformed ids ===")
    for command in ["cancelJob", "pauseJob", "resumeJob", "retryJob", "removeJob"]:
        r = core.call(command, {"jobId": "job-totally-unknown-id"})
        check(f"{command} on an unknown job id returns a clean error",
              r is not None and r.get("ok") is False)

    create_job_bad("setJobPriority on unknown job", None) if False else None
    r = core.call("setJobPriority", {"jobId": "nope", "priority": "HIGH"})
    check("setJobPriority on unknown job returns a clean error", r is not None and r.get("ok") is False)
    r = core.call("moveJob", {"jobId": "nope", "direction": "UP"})
    check("moveJob on unknown job returns a clean error", r is not None and r.get("ok") is False)
    r = core.call("moveJob", {"jobId": "nope", "direction": "SIDEWAYS"})
    check("moveJob with an invalid direction enum returns a clean error", r is not None and r.get("ok") is False)

    fuzz_case(core, "jobId with control characters", b'{"id": "1", "command": "cancelJob", "params": {"jobId": "abc\\u0001def"}}')
    fuzz_case(core, "jobId 10000 characters long", json.dumps(
        {"id": "1", "command": "cancelJob", "params": {"jobId": "a" * 10000}}).encode())
    fuzz_case(core, "jobId is an empty string", b'{"id": "1", "command": "cancelJob", "params": {"jobId": ""}}')
    fuzz_case(core, "jobId is a number, not a string", b'{"id": "1", "command": "cancelJob", "params": {"jobId": 5}}')

    print("\n=== 6. setConcurrency: bounds ===")
    for bad in [0, -1, -999999999, 99999999999, 1.5]:
        r = core.call("setConcurrency", {"maxConcurrency": bad})
        check(f"setConcurrency({bad}) rejected", r is not None and r.get("ok") is False)
    r = core.call("setConcurrency", {"maxConcurrency": "four"})
    check("setConcurrency with a string value rejected", r is not None and r.get("ok") is False)

    print("\n=== 7. clearHistory: invalid scope ===")
    r = core.call("clearHistory", {"scope": "EVERYTHING_INCLUDING_THE_KITCHEN_SINK"})
    check("clearHistory with an invalid scope rejected", r is not None and r.get("ok") is False)

    print("\n=== 8. Repeated identical requests / rapid-fire (no state corruption) ===")
    for _ in range(50):
        core.call("listJobs", {}, timeout=2)
    check("50 rapid identical listJobs calls: still alive and coherent", core.alive() and core.stream_is_coherent())

    print("\n=== 9. Same request id reused (must not desync the response table) ===")
    r1 = core.call("listJobs", {}, req_id="reused-id")
    r2 = core.call("getSettings", {}, req_id="reused-id")
    check("reusing a request id doesn't crash or hang", r1 is not None and r2 is not None)

    print("\n=== 10. inspectFile / getCapabilities with hostile paths ===")
    for path in ["../../../../etc/passwd", "/dev/null", "", "C:\\Windows\\System32\\config\\SAM",
                 "a" * 5000, "path/with\x00nul"]:
        r = core.call("inspectFile", {"path": path})
        check(f"inspectFile({path[:40]!r}...) handled without crashing", r is not None)
    check("stream still coherent after hostile-path batch", core.stream_is_coherent())

    print("\n=== 11. updateSettings with malformed settings payloads ===")
    create_job_bad("updateSettings missing settings key", None) if False else None
    r = core.call("updateSettings", {"settings": "not an object"})
    check("updateSettings with settings=string rejected cleanly", r is not None and r.get("ok") is False)
    r = core.call("updateSettings", {"settings": {"processing": {"concurrentJobs": -50}}})
    check("updateSettings with a negative concurrentJobs handled without crashing", r is not None)
    check("stream still coherent after settings batch", core.stream_is_coherent())

    print("\n=== 12. Final liveness check ===")
    check("core process still running after the entire fuzz battery", core.alive())
    with core.lock:
        malformed_count = len(core.malformed_lines)
    check("zero malformed lines ever appeared on stdout (protocol never corrupted)", malformed_count == 0,
          f"{malformed_count} malformed line(s)" if malformed_count else "")

    core.shutdown()

    print(f"\n{'='*60}\nPASSED {len(PASS)} / {len(PASS) + len(FAIL)}")
    if FAIL:
        print("FAILED:")
        for name in FAIL:
            print(f"  - {name}")
    return 0 if not FAIL else 1


if __name__ == "__main__":
    sys.exit(main())
