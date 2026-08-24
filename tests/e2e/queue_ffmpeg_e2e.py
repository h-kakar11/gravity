#!/usr/bin/env python3
"""Drives the real mediatool-core binary over its real NDJSON stdio protocol.

Nothing here is mocked: a real child process, real ffmpeg/ffprobe, real media files on
disk. Verifies the Phase 5 queue behaviours end to end.
"""
import json
import os
import shutil
import subprocess
import sys
import threading
import time

CORE = os.path.abspath("build/linux-debug/app/core/mediatool-core")
WORK = "/tmp/gravity-e2e-ipc"
STATE_DIR = os.path.join(WORK, "appdata")


class Core:
    def __init__(self, label):
        self.label = label
        env = dict(os.environ)
        env["LOCALAPPDATA"] = STATE_DIR
        self.proc = subprocess.Popen(
            [CORE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, bufsize=1, env=env)
        self.responses = {}
        self.events = []
        self.lock = threading.Lock()
        self.next_id = 0
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.proc.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                print(f"  !! NON-JSON ON STDOUT: {line[:200]}")
                raise SystemExit(1)
            with self.lock:
                if "event" in msg:
                    self.events.append(msg)
                elif "id" in msg:
                    self.responses[msg["id"]] = msg

    def call(self, command, params=None, timeout=90):
        self.next_id += 1
        rid = str(self.next_id)
        self.proc.stdin.write(json.dumps(
            {"id": rid, "command": command, "params": params or {}}) + "\n")
        self.proc.stdin.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                if rid in self.responses:
                    return self.responses.pop(rid)
            time.sleep(0.01)
        raise TimeoutError(f"{command} timed out")

    def ok(self, command, params=None, timeout=90):
        r = self.call(command, params, timeout)
        if not r.get("ok"):
            raise AssertionError(f"{command} failed: {r.get('error')}")
        return r["result"]

    def expect_error(self, command, params=None):
        r = self.call(command, params)
        assert not r.get("ok"), f"{command} unexpectedly succeeded"
        return r["error"]

    def wait_states(self, ids, terminal={"COMPLETED", "FAILED", "CANCELLED", "SKIPPED"}, timeout=180):
        deadline = time.time() + timeout
        while time.time() < deadline:
            snap = self.ok("getQueueSnapshot")["queue"]
            by_id = {j["id"]: j for j in snap["jobs"]}
            if all(by_id.get(i, {}).get("state") in terminal for i in ids):
                return by_id
            time.sleep(0.1)
        snap = self.ok("getQueueSnapshot")["queue"]
        by_id = {j["id"]: j for j in snap["jobs"]}
        raise TimeoutError("still not terminal: " +
                           str({i: by_id.get(i, {}).get("state") for i in ids}))

    def event_names(self):
        with self.lock:
            return [e["event"] for e in self.events]

    def close(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        self.proc.wait(timeout=30)


PASS, FAIL = [], []


def check(name, condition, detail=""):
    (PASS if condition else FAIL).append(name)
    print(f"  [{'PASS' if condition else 'FAIL'}] {name}{(' -- ' + detail) if detail else ''}")


def make_clip(path, seconds=2, size="320x240"):
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                    "-f", "lavfi", "-i", f"testsrc=size={size}:rate=15:duration={seconds}",
                    "-f", "lavfi", "-i", f"sine=frequency=440:duration={seconds}",
                    "-c:v", "libx264", "-c:a", "aac", "-shortest", path], check=True)


def main():
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(os.path.join(WORK, "src"), exist_ok=True)
    os.makedirs(os.path.join(WORK, "out"), exist_ok=True)
    os.makedirs(STATE_DIR, exist_ok=True)

    sources = []
    for i in range(4):
        p = os.path.join(WORK, "src", f"clip{i}.mp4")
        make_clip(p, seconds=2)
        sources.append(p)
    long_clip = os.path.join(WORK, "src", "long.mp4")
    make_clip(long_clip, seconds=25, size="640x480")

    out = os.path.join(WORK, "out")

    print("\n=== 1. Queue spans all three job types with real FFmpeg ===")
    c = Core("main")
    caps = c.ok("getProcessingCapabilities")
    check("ffmpeg discovered by core", caps["ffmpegAvailable"], str(caps["ffmpegAvailable"]))

    conv = c.ok("createJob", {"type": "CONVERSION", "params": {
        "inputPath": sources[0], "outputDirectory": out, "targetFormat": "MP3"}})["jobId"]
    comp = c.ok("createJob", {"type": "COMPRESSION", "params": {
        "inputPath": sources[1], "outputDirectory": out, "preset": "LOW", "maxHeight": 120}})["jobId"]
    by_id = c.wait_states([conv, comp])
    check("conversion completed", by_id[conv]["state"] == "COMPLETED", by_id[conv]["state"])
    check("compression completed", by_id[comp]["state"] == "COMPLETED", by_id[comp]["state"])
    conv_out = by_id[conv].get("result", {}).get("outputPath", "")
    comp_out = by_id[comp].get("result", {}).get("outputPath", "")
    check("conversion produced a real mp3", conv_out.endswith(".mp3") and os.path.getsize(conv_out) > 1000,
          f"{conv_out} {os.path.getsize(conv_out) if os.path.exists(conv_out) else 'MISSING'}")
    check("compression produced a real mp4", os.path.exists(comp_out) and os.path.getsize(comp_out) > 1000)
    probe = subprocess.run(["ffprobe", "-v", "quiet", "-print_format", "json",
                            "-show_streams", comp_out], capture_output=True, text=True)
    streams = json.loads(probe.stdout)["streams"]
    height = next((s["height"] for s in streams if s["codec_type"] == "video"), None)
    check("compression actually downscaled to 120px", height == 120, f"height={height}")
    check("no .processing temp files left behind",
          not any(".processing" in f for f in os.listdir(out)), str(os.listdir(out)))

    print("\n=== 2. Job metadata is populated ===")
    md = by_id[conv].get("metadata", {})
    check("conversion metadata has input/output filenames",
          "inputFilename" in md and "outputFilename" in md, str(sorted(md.keys())))
    check("conversion metadata has source and target format",
          md.get("sourceFormat") == "mp4" and md.get("targetFormat") == "mp3", str(md.get("targetFormat")))
    check("metadata does not leak an ffmpeg command line",
          not any("ffmpeg" in str(v).lower() or "-crf" in str(v) for v in md.values()), str(md))

    print("\n=== 3. Dependency pipeline: convert -> compress ===")
    step1 = c.ok("createJob", {"type": "CONVERSION", "params": {
        "inputPath": sources[2], "outputDirectory": out,
        "targetFormat": "MKV", "maxHeight": 180}})["jobId"]
    # The compression consumes the conversion's output path, which only exists once step1 ran.
    step1_out = os.path.join(out, "clip2.mkv")
    step2 = c.ok("createJob", {"type": "COMPRESSION", "params": {
        "inputPath": step1_out, "outputDirectory": out,
        "preset": "MEDIUM", "outputExtension": "mp4"},
        "dependsOn": [step1]})["jobId"]
    snap = c.ok("getQueueSnapshot")["queue"]
    step2_state = next(j["state"] for j in snap["jobs"] if j["id"] == step2)
    check("dependent starts in WAITING", step2_state in ("WAITING",), step2_state)
    by_id = c.wait_states([step1, step2])
    check("dependency ran first and both completed",
          by_id[step1]["state"] == "COMPLETED" and by_id[step2]["state"] == "COMPLETED",
          f"{by_id[step1]['state']}/{by_id[step2]['state']}")
    check("pipeline consumed the first job's output",
          os.path.exists(step1_out) and os.path.exists(by_id[step2]["result"]["outputPath"]))

    print("\n=== 4. Failed dependency skips the dependent ===")
    bad = c.ok("createJob", {"type": "CONVERSION", "params": {
        "inputPath": os.path.join(WORK, "src", "nope.mp4"),
        "outputDirectory": out, "targetFormat": "MP3"}})["jobId"]
    dependent = c.ok("createJob", {"type": "CONVERSION", "params": {
        "inputPath": sources[3], "outputDirectory": out, "targetFormat": "WAV"},
        "dependsOn": [bad]})["jobId"]
    by_id = c.wait_states([bad, dependent])
    check("missing input fails permanently", by_id[bad]["state"] == "FAILED", by_id[bad]["state"])
    check("failure code is INPUT_NOT_FOUND",
          by_id[bad].get("error", {}).get("code") == "E_INPUT_NOT_FOUND",
          str(by_id[bad].get("error", {}).get("code")))
    check("dependent is SKIPPED not FAILED", by_id[dependent]["state"] == "SKIPPED",
          by_id[dependent]["state"])
    check("skip reason names the dependency",
          by_id[dependent].get("error", {}).get("code") == "E_DEPENDENCY_FAILED")
    check("no automatic retry for a permanent failure", by_id[bad]["retryCount"] == 0,
          str(by_id[bad]["retryCount"]))

    print("\n=== 5. Duplicate detection ===")
    dup_params = {"type": "CONVERSION", "params": {
        "inputPath": sources[0], "outputDirectory": out, "targetFormat": "FLAC"}}
    first = c.ok("createJob", dup_params)["jobId"]
    err = c.expect_error("createJob", dup_params)
    check("identical pending request is rejected", err["code"] == "E_DUPLICATE_JOB", err["code"])
    check("rejection names the existing job", first in err.get("details", ""), err.get("details"))
    allowed = dict(dup_params, allowDuplicate=True)
    second = c.ok("createJob", allowed)["jobId"]
    check("explicit allowDuplicate creates a second job", second != first)
    different = c.ok("createJob", {"type": "CONVERSION", "params": {
        "inputPath": sources[0], "outputDirectory": out, "targetFormat": "WAV"}})["jobId"]
    check("a different target format is not a duplicate", different not in (first, second))
    c.wait_states([first, second, different])

    print("\n=== 6. Concurrency limit is respected by real processes ===")
    c.ok("setConcurrency", {"maxConcurrency": 2})
    c.ok("pauseQueue")
    long_ids = []
    for i in range(5):
        long_ids.append(c.ok("createJob", {"type": "COMPRESSION", "params": {
            "inputPath": long_clip, "outputDirectory": out,
            "preset": "HIGH", "outputFilenameBase": f"conc{i}"}})["jobId"])
    snap = c.ok("getQueueSnapshot")["queue"]
    check("paused queue started nothing", snap["statistics"]["running"] == 0,
          str(snap["statistics"]))
    check("all five are queued", snap["statistics"]["queued"] == 5, str(snap["statistics"]))

    c.ok("resumeQueue")
    peak_running = 0
    peak_ffmpeg = 0
    deadline = time.time() + 180
    while time.time() < deadline:
        snap = c.ok("getQueueSnapshot")["queue"]
        peak_running = max(peak_running, snap["statistics"]["running"])
        # Count real ffmpeg children of this core process, not just job objects.
        n = subprocess.run(["pgrep", "-c", "-P", str(c.proc.pid), "ffmpeg"],
                           capture_output=True, text=True).stdout.strip()
        try:
            peak_ffmpeg = max(peak_ffmpeg, int(n))
        except ValueError:
            pass
        if all(j["state"] in ("COMPLETED", "FAILED") for j in snap["jobs"] if j["id"] in long_ids):
            break
        time.sleep(0.05)
    check("running count never exceeded the limit of 2", peak_running <= 2, f"peak={peak_running}")
    check("real ffmpeg process count never exceeded 2", peak_ffmpeg <= 2, f"peak={peak_ffmpeg}")
    by_id = c.wait_states(long_ids)
    check("all five eventually completed",
          all(by_id[i]["state"] == "COMPLETED" for i in long_ids),
          str([by_id[i]["state"] for i in long_ids]))

    print("\n=== 7. Cancellation of a real running ffmpeg ===")
    c.ok("setConcurrency", {"maxConcurrency": 1})
    cancel_id = c.ok("createJob", {"type": "COMPRESSION", "params": {
        "inputPath": long_clip, "outputDirectory": out,
        "preset": "HIGH", "outputFilenameBase": "tocancel"}})["jobId"]
    deadline = time.time() + 60
    while time.time() < deadline:
        st = c.ok("getJob", {"jobId": cancel_id})["job"]["state"]
        if st == "RUNNING":
            break
        time.sleep(0.05)
    check("job reached RUNNING before cancel", st == "RUNNING", st)
    c.ok("cancelJob", {"jobId": cancel_id})
    by_id = c.wait_states([cancel_id])
    check("cancelled job ends CANCELLED", by_id[cancel_id]["state"] == "CANCELLED",
          by_id[cancel_id]["state"])
    time.sleep(1.0)
    leftover = [f for f in os.listdir(out) if f.startswith("tocancel")]
    check("cancellation left no partial output", leftover == [], str(leftover))
    n = subprocess.run(["pgrep", "-c", "-P", str(c.proc.pid), "ffmpeg"],
                       capture_output=True, text=True).stdout.strip()
    check("the ffmpeg child was actually killed", n in ("", "0"), f"remaining={n}")

    print("\n=== 8. Cancel while queued never starts the process ===")
    c.ok("pauseQueue")
    queued_cancel = c.ok("createJob", {"type": "COMPRESSION", "params": {
        "inputPath": long_clip, "outputDirectory": out,
        "preset": "LOW", "outputFilenameBase": "neverstarted"}})["jobId"]
    c.ok("cancelJob", {"jobId": queued_cancel})
    st = c.ok("getJob", {"jobId": queued_cancel})["job"]["state"]
    check("queued cancel is immediate", st == "CANCELLED", st)
    c.ok("resumeQueue")
    time.sleep(1.0)
    check("cancelled-while-queued produced no output",
          not any(f.startswith("neverstarted") for f in os.listdir(out)))

    print("\n=== 9. Priority and reordering ===")
    c.ok("pauseQueue")
    order_ids = []
    for i in range(4):
        order_ids.append(c.ok("createJob", {"type": "CONVERSION", "params": {
            "inputPath": sources[i % 4], "outputDirectory": out,
            "targetFormat": "WAV", "outputFilenameBase": f"order{i}"}})["jobId"])
    snap = c.ok("getQueueSnapshot")["queue"]
    pending = [i for i in snap["pendingOrder"] if i in order_ids]
    check("pending order is FIFO", pending == order_ids, str(pending == order_ids))
    c.ok("moveJob", {"jobId": order_ids[3], "direction": "TOP"})
    snap = c.ok("getQueueSnapshot")["queue"]
    pending = [i for i in snap["pendingOrder"] if i in order_ids]
    check("move to top reorders", pending[0] == order_ids[3])
    c.ok("setJobPriority", {"jobId": order_ids[1], "priority": "HIGH"})
    job = c.ok("getJob", {"jobId": order_ids[1]})["job"]
    check("priority is reflected in the snapshot", job["priority"] == "HIGH", job["priority"])
    err = c.expect_error("moveJob", {"jobId": "job-nope", "direction": "TOP"})
    check("moving an unknown job is rejected", err["code"] == "E_JOB_NOT_FOUND", err["code"])
    c.ok("resumeQueue")
    c.wait_states(order_ids)

    print("\n=== 10. Input validation rejects bad IPC input ===")
    cases = [
        ("unknown command", "notARealCommand", {}, "E_UNKNOWN_COMMAND"),
        ("missing jobId", "cancelJob", {}, "E_INVALID_PARAMS"),
        ("non-string jobId", "cancelJob", {"jobId": 42}, "E_INVALID_PARAMS"),
        ("unknown job", "cancelJob", {"jobId": "job-nope"}, "E_JOB_NOT_FOUND"),
        ("bad priority", "setJobPriority",
         {"jobId": order_ids[0], "priority": "URGENT"}, "E_INVALID_PRIORITY"),
        ("bad direction", "moveJob",
         {"jobId": order_ids[0], "direction": "SIDEWAYS"}, "E_INVALID_MOVE_DIRECTION"),
        ("concurrency out of range", "setConcurrency", {"maxConcurrency": 9999}, "E_INVALID_PARAMS"),
        ("negative concurrency", "setConcurrency", {"maxConcurrency": -1}, "E_INVALID_PARAMS"),
        ("bad history scope", "clearHistory", {"scope": "EVERYTHING"}, "E_INVALID_HISTORY_SCOPE"),
        ("unknown job type", "createJob", {"type": "NONSENSE", "params": {}}, "E_INVALID_PARAMS"),
        ("path traversal", "createJob", {"type": "CONVERSION", "params": {
            "inputPath": "../../etc/passwd", "outputDirectory": out,
            "targetFormat": "MP3"}}, "E_INVALID_PARAMS"),
        ("unsupported target format", "createJob", {"type": "CONVERSION", "params": {
            "inputPath": sources[0], "outputDirectory": out,
            "targetFormat": "REALMEDIA"}}, "E_UNSUPPORTED_TARGET_FORMAT"),
        ("out-of-range option", "createJob", {"type": "COMPRESSION", "params": {
            "inputPath": sources[0], "outputDirectory": out,
            "maxHeight": 999999}}, "E_INVALID_PROCESSING_OPTION"),
        ("unknown dependency", "createJob", {"type": "CONVERSION", "params": {
            "inputPath": sources[0], "outputDirectory": out, "targetFormat": "MP3"},
            "dependsOn": ["job-nope"]}, "E_JOB_NOT_FOUND"),
        ("bad retry policy", "createJob", {"type": "CONVERSION", "params": {
            "inputPath": sources[0], "outputDirectory": out, "targetFormat": "MP3"},
            "retryPolicy": {"maxRetries": 9999}}, "E_INVALID_RETRY_POLICY"),
    ]
    for label, cmd, params, expected in cases:
        err = c.expect_error(cmd, params)
        check(f"rejects: {label}", err["code"] == expected, f"got {err['code']}")

    print("\n=== 11. Events are coherent and sequenced ===")
    names = c.event_names()
    with c.lock:
        seqs = [e["seq"] for e in c.events if "seq" in e]
    check("saw jobCreated/jobStarted/jobCompleted",
          {"jobCreated", "jobStarted", "jobCompleted"} <= set(names))
    check("saw jobSkipped for the dependency failure", "jobSkipped" in names)
    check("saw queueChanged", "queueChanged" in names)
    check("saw jobProgress", "jobProgress" in names)
    check("every event carries a sequence number", len(seqs) == len(names),
          f"{len(seqs)} of {len(names)}")
    # Sequence is stamped under the same lock that writes the line, so arrival order and
    # sequence order are the same thing -- that is exactly what the frontend relies on.
    check("event sequence matches wire arrival order",
          all(b > a for a, b in zip(seqs, seqs[1:])), f"{len(seqs)} sequenced events")
    progress_events = sum(1 for n in names if n == "jobProgress")
    check("progress events are throttled, not one per ffmpeg tick",
          progress_events < 4000, f"{progress_events} progress events total")

    print("\n=== 12. Clear history keeps files and live jobs ===")
    files_before = set(os.listdir(out))
    stats_before = c.ok("getQueueSnapshot")["queue"]["statistics"]
    removed = c.ok("clearHistory", {"scope": "COMPLETED"})["removedCount"]
    stats_after = c.ok("getQueueSnapshot")["queue"]["statistics"]
    check("clearing completed removed entries", removed > 0, f"removed={removed}")
    check("completed count is now zero", stats_after["completed"] == 0, str(stats_after))
    check("failures were kept", stats_after["failed"] == stats_before["failed"])
    check("no media files were deleted", set(os.listdir(out)) == files_before)

    print("\n=== 13. Restart recovery ===")
    c.ok("pauseQueue")
    survivor = c.ok("createJob", {"type": "CONVERSION", "params": {
        "inputPath": sources[0], "outputDirectory": out,
        "targetFormat": "M4A", "outputFilenameBase": "survivor"},
        "priority": "HIGH"})["jobId"]
    time.sleep(1.0)  # let the throttled persist tick land
    c.close()

    state_file = os.path.join(STATE_DIR, "Gravity", "queue.json")
    check("queue state file was written", os.path.exists(state_file), state_file)
    with open(state_file) as f:
        persisted = json.load(f)
    check("state file is versioned", persisted.get("schemaVersion") == 1,
          str(persisted.get("schemaVersion")))
    check("state file records the paused queue", persisted.get("runState") == "PAUSED")
    check("no process handles or PIDs were persisted",
          not any(k in json.dumps(persisted).lower() for k in ("\"pid\"", "processhandle")))

    c2 = Core("restarted")
    snap = c2.ok("getQueueSnapshot")["queue"]
    by_id = {j["id"]: j for j in snap["jobs"]}
    check("the queued job survived the restart", survivor in by_id, str(list(by_id)[:3]))
    check("it kept its id and priority",
          by_id.get(survivor, {}).get("priority") == "HIGH",
          str(by_id.get(survivor, {}).get("priority")))
    check("the paused queue state survived", snap["runState"] == "PAUSED", snap["runState"])
    c2.ok("resumeQueue")
    by_id = c2.wait_states([survivor])
    check("the restored job runs to completion after restart",
          by_id[survivor]["state"] == "COMPLETED", by_id[survivor]["state"])
    check("it produced real output",
          os.path.exists(by_id[survivor]["result"]["outputPath"]))

    print("\n=== 14. Corrupt state file recovery ===")
    c2.close()
    with open(state_file, "w") as f:
        f.write('{"schemaVersion": 1, "records": [{"id": "x", trunca')
    c3 = Core("after-corruption")
    snap = c3.ok("getQueueSnapshot")["queue"]
    check("app starts with a corrupt state file", snap["statistics"]["total"] == 0,
          str(snap["statistics"]))
    quarantined = [f for f in os.listdir(os.path.dirname(state_file)) if ".corrupt-" in f]
    check("the corrupt file was preserved for diagnosis", len(quarantined) == 1, str(quarantined))
    still = c3.ok("createJob", {"type": "CONVERSION", "params": {
        "inputPath": sources[0], "outputDirectory": out,
        "targetFormat": "WAV", "outputFilenameBase": "afterCorrupt"}})["jobId"]
    by_id = c3.wait_states([still])
    check("the queue is fully usable afterwards", by_id[still]["state"] == "COMPLETED",
          by_id[still]["state"])
    c3.close()

    print(f"\n{'=' * 60}")
    print(f"PASSED {len(PASS)} / {len(PASS) + len(FAIL)}")
    if FAIL:
        print("FAILED:")
        for name in FAIL:
            print(f"  - {name}")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
