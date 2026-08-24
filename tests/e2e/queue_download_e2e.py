#!/usr/bin/env python3
"""Real end-to-end verification of the DOWNLOAD path and automatic retry.

Real mediatool-core binary, real child-process launching through the real
YtDlpProvider, real NDJSON protocol, real ffprobe verification of the result. Only
yt-dlp itself is stood in for (see fake_downloader.py), so this needs no network and
is reproducible -- which is exactly what spec section 50 asks for.
"""
import json
import os
import shutil
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CORE = os.path.abspath("build/linux-debug/app/core/mediatool-core")
WORK = "/tmp/gravity-e2e-download"
STATE_DIR = os.path.join(WORK, "appdata")
PASS, FAIL = [], []


def check(name, condition, detail=""):
    (PASS if condition else FAIL).append(name)
    print(f"  [{'PASS' if condition else 'FAIL'}] {name}{(' -- ' + detail) if detail else ''}")


class Core:
    def __init__(self, **extra_env):
        env = dict(os.environ)
        env["LOCALAPPDATA"] = STATE_DIR
        env["MEDIATOOL_PYTHON_PATH"] = sys.executable
        env["MEDIATOOL_DOWNLOADER_SCRIPT"] = os.path.join(HERE, "fake_downloader.py")
        env.update(extra_env)
        self.proc = subprocess.Popen(
            [CORE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, bufsize=1, env=env)
        self.responses, self.events = {}, []
        self.lock = threading.Lock()
        self.next_id = 0
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            line = line.strip()
            if not line:
                continue
            msg = json.loads(line)
            with self.lock:
                if "event" in msg:
                    self.events.append(msg)
                elif "id" in msg:
                    self.responses[msg["id"]] = msg

    def call(self, command, params=None, timeout=120):
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
        raise TimeoutError(command)

    def ok(self, command, params=None, timeout=120):
        r = self.call(command, params, timeout)
        if not r.get("ok"):
            raise AssertionError(f"{command} failed: {r.get('error')}")
        return r["result"]

    def job(self, job_id):
        return self.ok("getJob", {"jobId": job_id})["job"]

    def wait_terminal(self, ids, timeout=240):
        terminal = {"COMPLETED", "FAILED", "CANCELLED", "SKIPPED"}
        deadline = time.time() + timeout
        while time.time() < deadline:
            snap = self.ok("getQueueSnapshot")["queue"]
            by_id = {j["id"]: j for j in snap["jobs"]}
            if all(by_id.get(i, {}).get("state") in terminal for i in ids):
                return by_id
            time.sleep(0.1)
        raise TimeoutError(str({i: by_id.get(i, {}).get("state") for i in ids}))

    def event_names(self):
        with self.lock:
            return [e["event"] for e in self.events]

    def close(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        self.proc.wait(timeout=30)


def main():
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(os.path.join(WORK, "out"), exist_ok=True)
    os.makedirs(STATE_DIR, exist_ok=True)
    out = os.path.join(WORK, "out")

    print("\n=== 1. Real download jobs queue and complete ===")
    c = Core(FAKE_DL_STATE=os.path.join(WORK, "attempts-a.txt"), FAKE_DL_FAIL_TIMES="0")
    meta = c.ok("inspectDownloadUrl", {"url": "https://example.com/watch?v=a"})["metadata"]
    check("inspect reaches the real provider", meta["title"] == "Fake Clip", meta["title"])

    # Concurrency deliberately > 1 and every download reporting the SAME title, which is the
    # case that used to make all three write to one file and report success anyway.
    c.ok("setConcurrency", {"maxConcurrency": 3})
    ids = []
    for i in range(3):
        ids.append(c.ok("createJob", {"type": "DOWNLOAD", "params": {
            "url": f"https://example.com/watch?v={i}", "outputDirectory": out,
            "quality": "BEST"}})["jobId"])
    by_id = c.wait_terminal(ids)
    check("all three downloads completed",
          all(by_id[i]["state"] == "COMPLETED" for i in ids),
          str([by_id[i]["state"] for i in ids]))
    for i in ids:
        path = by_id[i].get("result", {}).get("outputPath", "")
        check(f"produced a verified file for {i[:12]}",
              os.path.exists(path) and os.path.getsize(path) > 1000)
    check("download metadata carries the title",
          by_id[ids[0]].get("metadata", {}).get("title") == "Fake Clip")

    produced = [by_id[i]["result"]["outputPath"] for i in ids]
    check("three concurrent same-title downloads produced three distinct files",
          len(set(produced)) == 3, str(sorted(os.path.basename(p) for p in produced)))
    check("all three files really exist on disk",
          all(os.path.exists(p) and os.path.getsize(p) > 1000 for p in produced),
          str(sorted(os.listdir(out))))
    c.close()

    print("\n=== 2. A transient download failure is retried automatically ===")
    state = os.path.join(WORK, "attempts-b.txt")
    c = Core(FAKE_DL_STATE=state, FAKE_DL_FAIL_TIMES="2",
             FAKE_DL_FAIL_CODE="E_DOWNLOAD_TRANSPORT_ERROR",
             FAKE_DL_FAIL_CATEGORY="NETWORK_ERROR")
    job_id = c.ok("createJob", {"type": "DOWNLOAD", "params": {
        "url": "https://example.com/watch?v=flaky", "outputDirectory": out,
        "quality": "BEST", "outputFilenameBase": "flaky"},
        "retryPolicy": {"maxRetries": 3, "initialDelayMs": 300,
                        "maxDelayMs": 2000, "multiplier": 2.0}})["jobId"]

    # It must pass through RETRY_WAIT before succeeding -- a job that simply took a while
    # would not prove the retry machinery ran.
    saw_retry_wait = False
    deadline = time.time() + 120
    while time.time() < deadline:
        st = c.job(job_id)["state"]
        if st == "RETRY_WAIT":
            saw_retry_wait = True
        if st in ("COMPLETED", "FAILED", "CANCELLED"):
            break
        time.sleep(0.02)

    final = c.job(job_id)
    check("job entered RETRY_WAIT between attempts", saw_retry_wait)
    check("it eventually completed", final["state"] == "COMPLETED", final["state"])
    check("it recorded 2 retries", final["retryCount"] == 2, str(final["retryCount"]))
    with open(state) as f:
        attempts = int(f.read().strip())
    check("the downloader really ran 3 times", attempts == 3, str(attempts))
    check("the retry reason names the classification",
          "transient" in (final.get("retryReason") or "").lower(),
          final.get("retryReason"))
    check("output exists and was ffprobe-verified",
          os.path.exists(final["result"]["outputPath"]))
    names = c.event_names()
    check("a jobRetryScheduled event was published", "jobRetryScheduled" in names)

    print("\n=== 3. Retries stop at the budget ===")
    state2 = os.path.join(WORK, "attempts-c.txt")
    c2 = Core(FAKE_DL_STATE=state2, FAKE_DL_FAIL_TIMES="99",
              FAKE_DL_FAIL_CODE="E_DOWNLOAD_TRANSPORT_ERROR",
              FAKE_DL_FAIL_CATEGORY="NETWORK_ERROR")
    doomed = c2.ok("createJob", {"type": "DOWNLOAD", "params": {
        "url": "https://example.com/watch?v=doomed", "outputDirectory": out,
        "quality": "BEST", "outputFilenameBase": "doomed"},
        "retryPolicy": {"maxRetries": 2, "initialDelayMs": 200,
                        "maxDelayMs": 1000, "multiplier": 2.0}})["jobId"]
    by_id = c2.wait_terminal([doomed])
    with open(state2) as f:
        attempts = int(f.read().strip())
    check("gives up after the budget", by_id[doomed]["state"] == "FAILED", by_id[doomed]["state"])
    check("ran exactly 1 + 2 times, not forever", attempts == 3, str(attempts))
    check("retryCount stopped at maxRetries", by_id[doomed]["retryCount"] == 2,
          str(by_id[doomed]["retryCount"]))
    check("no partial output was left behind",
          not any(f.startswith("doomed") for f in os.listdir(out)),
          str([f for f in os.listdir(out) if f.startswith("doomed")]))
    c2.close()

    print("\n=== 4. A permanent download failure is not retried ===")
    state3 = os.path.join(WORK, "attempts-d.txt")
    c3 = Core(FAKE_DL_STATE=state3, FAKE_DL_FAIL_TIMES="99",
              FAKE_DL_FAIL_CODE="E_DOWNLOAD_NOT_FOUND",
              FAKE_DL_FAIL_CATEGORY="NETWORK_ERROR")
    gone = c3.ok("createJob", {"type": "DOWNLOAD", "params": {
        "url": "https://example.com/watch?v=gone", "outputDirectory": out,
        "quality": "BEST"},
        "retryPolicy": {"maxRetries": 5, "initialDelayMs": 100,
                        "maxDelayMs": 500, "multiplier": 2.0}})["jobId"]
    by_id = c3.wait_terminal([gone])
    time.sleep(1.5)  # give any wrongly-scheduled retry time to fire
    with open(state3) as f:
        attempts = int(f.read().strip())
    check("a removed video fails immediately", by_id[gone]["state"] == "FAILED")
    # E_DOWNLOAD_NOT_FOUND arrives in a retryable *category* but is permanent by code --
    # this is the code-level override doing its job against the real pipeline.
    check("it was tried exactly once", attempts == 1, str(attempts))
    check("retryCount stayed at zero", c3.job(gone)["retryCount"] == 0)
    c3.close()

    print("\n=== 5. Manual retry of a failed download ===")
    # The same core process the doomed job ran in is gone; use a fresh one whose fake
    # downloader now succeeds, and retry by hand.
    state4 = os.path.join(WORK, "attempts-e.txt")
    with open(state4, "w") as f:
        f.write("0")
    c4 = Core(FAKE_DL_STATE=state4, FAKE_DL_FAIL_TIMES="1",
              FAKE_DL_FAIL_CODE="E_DOWNLOAD_NOT_FOUND",  # permanent: no auto-retry
              FAKE_DL_FAIL_CATEGORY="NETWORK_ERROR")
    manual = c4.ok("createJob", {"type": "DOWNLOAD", "params": {
        "url": "https://example.com/watch?v=manual", "outputDirectory": out,
        "quality": "BEST"}})["jobId"]
    by_id = c4.wait_terminal([manual])
    check("first attempt failed permanently", by_id[manual]["state"] == "FAILED")

    c4.ok("retryJob", {"jobId": manual})
    by_id = c4.wait_terminal([manual])
    final = c4.job(manual)
    check("manual retry ran it again and it succeeded", final["state"] == "COMPLETED",
          final["state"])
    check("the job kept its original id", final["id"] == manual)
    check("manual retry is counted as an attempt", final["retryCount"] >= 1,
          str(final["retryCount"]))
    produced = final["result"]["outputPath"]
    check("the retry produced a real, verified file",
          os.path.exists(produced) and os.path.getsize(produced) > 1000, produced)

    print("\n=== 6. Download -> convert -> compress, linked by dependencies ===")
    # No output path is guessed anywhere here. A download's filename comes from the media's
    # title and whichever container the extractor chose, so the only correct way to chain
    # onto it is to name the producing job and let the backend resolve the path once it has
    # actually run (spec section 19).
    src = c4.ok("createJob", {"type": "DOWNLOAD", "params": {
        "url": "https://example.com/watch?v=pipeline", "outputDirectory": out,
        "quality": "BEST"}})["jobId"]
    conv = c4.ok("createJob", {"type": "CONVERSION", "params": {
        "inputFromJobId": src, "outputDirectory": out,
        "targetFormat": "MKV", "outputFilenameBase": "pipeline-converted"}})["jobId"]
    comp = c4.ok("createJob", {"type": "COMPRESSION", "params": {
        "inputFromJobId": conv, "outputDirectory": out, "preset": "LOW",
        "outputFilenameBase": "pipeline-compressed", "outputExtension": "mp4"}})["jobId"]

    snap = c4.ok("getQueueSnapshot")["queue"]
    states = {j["id"]: j["state"] for j in snap["jobs"]}
    check("both dependents start blocked",
          states.get(conv) == "WAITING" and states.get(comp) == "WAITING",
          f"{states.get(conv)}/{states.get(comp)}")

    by_id = c4.wait_terminal([src, conv, comp])
    check("all three stages completed",
          all(by_id[i]["state"] == "COMPLETED" for i in (src, conv, comp)),
          str([by_id[i]["state"] for i in (src, conv, comp)]))
    downloaded = by_id[src]["result"]["outputPath"]
    converted = by_id[conv]["result"]["outputPath"]
    compressed = by_id[comp]["result"]["outputPath"]
    check("each stage consumed the previous stage's real output",
          os.path.exists(downloaded) and os.path.exists(converted) and os.path.exists(compressed))
    # The conversion's recorded input must be the path the download actually wrote -- proof
    # the backend resolved it rather than the caller having guessed correctly by luck.
    check("the conversion read the download's actual output",
          by_id[conv]["metadata"]["inputPath"] == downloaded,
          f'{by_id[conv]["metadata"].get("inputPath")} vs {downloaded}')
    check("the compression read the conversion's actual output",
          by_id[comp]["metadata"]["inputPath"] == converted,
          f'{by_id[comp]["metadata"].get("inputPath")} vs {converted}')
    check("declaring inputFromJobId implied the dependency",
          by_id[conv]["dependencies"] == [src] and by_id[comp]["dependencies"] == [conv],
          f'{by_id[conv]["dependencies"]} / {by_id[comp]["dependencies"]}')
    # Ordering is the real proof the dependency was honoured, not just the end state.
    check("they ran in dependency order",
          by_id[src]["completedAt"] <= by_id[conv]["startedAt"]
          and by_id[conv]["completedAt"] <= by_id[comp]["startedAt"],
          f"{by_id[src]['completedAt']} -> {by_id[conv]['startedAt']}")
    c4.close()

    print("\n=== 7. Cancellation of a real running download leaves no partial file ===")
    # Real Terminate()/Kill() escalation against a real child process (spec section 27),
    # mirroring the FFmpeg cancellation coverage in queue_ffmpeg_e2e.py -- the download
    # path has its own CleanupArtifacts() and deserves the same real-subprocess proof,
    # not just the mocked DownloadJobTest coverage.
    before = set(os.listdir(out))
    c5 = Core(FAKE_DL_HANG="1")
    cancel_id = c5.ok("createJob", {"type": "DOWNLOAD", "params": {
        "url": "https://example.com/watch?v=tocancel", "outputDirectory": out,
        "quality": "BEST"}})["jobId"]
    deadline = time.time() + 30
    st = None
    while time.time() < deadline:
        st = c5.job(cancel_id)["state"]
        if st == "RUNNING":
            break
        time.sleep(0.05)
    check("job reached RUNNING before cancel", st == "RUNNING", str(st))
    # Give the fake downloader a moment to actually write its .part artifact before
    # cancelling, so the cleanup this test verifies has something real to clean up.
    time.sleep(0.3)
    c5.ok("cancelJob", {"jobId": cancel_id})
    by_id = c5.wait_terminal([cancel_id])
    check("cancelled download ends CANCELLED", by_id[cancel_id]["state"] == "CANCELLED",
          by_id[cancel_id]["state"])
    leftover = set(os.listdir(out)) - before
    check("cancellation left no new file behind", leftover == set(), str(leftover))
    c5.close()

    print(f"\n{'=' * 60}")
    print(f"PASSED {len(PASS)} / {len(PASS) + len(FAIL)}")
    for name in FAIL:
        print(f"  FAILED: {name}")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
