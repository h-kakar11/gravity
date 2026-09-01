#include "core/process/RealProcessRunner.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <reproc++/reproc.hpp>

#include "core/errors/ErrorInfo.h"
#include "core/errors/MediaToolException.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace mediatool::process {

namespace {

// Accumulates raw bytes from one stream and invokes the line callback once complete
// '\n'-terminated lines are available. Only ever touched from the single drain thread,
// so it needs no locking of its own.
class LineSplitter {
public:
    explicit LineSplitter(OutputLineCallback callback) : callback_(std::move(callback)) {}

    std::error_code operator()(reproc::stream /*stream*/, const uint8_t* buffer, size_t size) {
        if (size > 0) {
            pending_.append(reinterpret_cast<const char*>(buffer), size);
        }
        // A child that never writes a newline (or writes one enormous line) would
        // otherwise let pending_ grow without bound for as long as the process runs --
        // issue #24. This doesn't change behavior for any real line-oriented protocol
        // this codebase actually parses (NDJSON, ffmpeg -progress, yt-dlp progress hooks
        // all write frequent newlines well under this size); it only stops a pathological
        // producer from growing this buffer forever. Emits what's accumulated so far as a
        // synthetic "line" and starts over, rather than silently dropping bytes.
        static constexpr std::size_t kMaxPendingBytes = 1 * 1024 * 1024;
        if (pending_.size() > kMaxPendingBytes) {
            std::string truncated = std::move(pending_);
            pending_.clear();
            if (callback_) {
                callback_(truncated + "...[truncated, no newline within " +
                           std::to_string(kMaxPendingBytes) + " bytes]");
            }
        }
        size_t newlinePos;
        while ((newlinePos = pending_.find('\n')) != std::string::npos) {
            std::string line = pending_.substr(0, newlinePos);
            pending_.erase(0, newlinePos + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (callback_) {
                callback_(line);
            }
        }
        return {};
    }

    // Called once drain() returns (stream closed / process exited) to deliver a final
    // line that never got a trailing newline.
    void Flush() {
        if (!pending_.empty()) {
            std::string line = std::move(pending_);
            pending_.clear();
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (callback_) {
                callback_(line);
            }
        }
    }

private:
    OutputLineCallback callback_;
    std::string pending_;
};

class RealProcess : public IProcess {
public:
    RealProcess(reproc::process&& process, OutputLineCallback onStdout, OutputLineCallback onStderr)
        : process_(std::move(process)) {
        // reproc does not support calling wait()/poll()/terminate()/kill() on the same
        // process handle concurrently from two threads. Empirically confirmed: a
        // single-threaded kill()-then-wait() completes in ~3ms, but calling kill() from
        // the caller's thread while a second thread is blocked inside reproc::drain()'s
        // poll() loop left the child running for the drain's full natural lifetime
        // instead of dying immediately (observed as Kill() + WaitFor(5000) taking ~30s
        // instead of returning promptly). The fix: ONLY this drain thread ever calls into
        // `process_`, for wait/poll/read AND terminate/kill. Terminate()/Kill() just set
        // `pendingAction_`; this loop polls with a short, finite timeout specifically so
        // it can notice that flag promptly and act on it itself, rather than blocking
        // forever in a single infinite-timeout poll like reproc::drain() does.
#ifdef _WIN32
        // Wrap the child in a Windows Job Object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        // (#9): reproc's own Kill() only ever terminates this one direct child, so a
        // grandchild the child itself spawned (e.g. yt-dlp launching its own ffmpeg for a
        // merge) survives a Kill() and is orphaned. Assigning the process to a job with
        // this flag means the whole descendant tree dies together, either when
        // TerminateJobObject() is called explicitly (see Kill() below) or when the last
        // handle to the job is closed (see the destructor) -- so even a caller that drops
        // this IProcess without calling Kill() can't leak the tree. Best-effort: if any
        // step here fails, Kill() falls back to reproc's single-process termination only,
        // same as before this fix existed.
        {
            auto [pid, pidEc] = process_.pid();
            if (!pidEc) {
                HANDLE job = CreateJobObjectW(nullptr, nullptr);
                if (job != nullptr) {
                    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
                    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                    bool assigned = false;
                    if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info,
                                                 sizeof(info))) {
                        HANDLE proc =
                            OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                                        static_cast<DWORD>(pid));
                        if (proc != nullptr) {
                            assigned = AssignProcessToJobObject(job, proc) != 0;
                            CloseHandle(proc);
                        }
                    }
                    if (assigned) {
                        jobObject_ = job;
                    } else {
                        CloseHandle(job);
                    }
                }
            }
        }
#endif

        drainThread_ = std::thread([this, onStdout = std::move(onStdout),
                                     onStderr = std::move(onStderr)]() mutable {
            LineSplitter outSink(std::move(onStdout));
            LineSplitter errSink(std::move(onStderr));
            static constexpr std::size_t kBufferSize = 4096;
            uint8_t buffer[kBufferSize];

            for (;;) {
                const int action = pendingAction_.exchange(0);
                if (action == 1) {
                    process_.terminate();
                } else if (action == 2) {
                    process_.kill();
                }
                // Stdin writes and the stdin close happen HERE, on this thread, for
                // exactly the reason terminate()/kill() already do: reproc does not
                // support two threads operating on one process handle. WriteLine() used
                // to call process_.write() directly from the caller's thread, and the
                // second such write to a live child fails with REPROC_EINVAL while this
                // loop is inside poll()/read() -- reproducible, and previously invisible
                // because the only production caller (YtDlpProvider) writes exactly one
                // command and then closes stdin, so a second write never happened. See
                // tests/integration/IpcProtocolTest.cpp, which does write repeatedly and
                // is what found it.
                RunPendingStdinWork();

                auto [events, pollEc] = process_.poll(reproc::event::out | reproc::event::err,
                                                       reproc::milliseconds(kPollIntervalMs));
                // reproc represents "our 200ms poll window elapsed with nothing ready" as
                // a SUCCESS code (empty pollEc) with events == 0, not as a distinct timeout
                // error -- reproc_poll() (reproc/src/reproc.c) only ever sets a DEADLINE
                // event when some other, earlier deadline cut the wait short, which isn't
                // the case here. A prior version of this check compared pollEc to
                // std::errc::timed_out, which reproc never actually returns for this path:
                // that made the check dead code, so execution fell through to the read()
                // below with events == 0, which resolves to reproc::stream::err and BLOCKS
                // on it -- meaning Kill()/Terminate() (which only set pendingAction_, read
                // back at the top of this loop) would not actually run until the child
                // produced output or exited on its own. See docs/pr43-findings.md.
                if (!pollEc && events == 0) {
                    continue;  // no output ready -- loop back around to re-check pendingAction_
                }
                if (pollEc == reproc::error::broken_pipe) {
                    break;  // both streams closed: the process exited (possibly just killed above)
                }
                if (pollEc) {
                    break;  // unexpected error -- stop draining, fall through to wait()
                }

                const reproc::stream which = (events & reproc::event::out) ? reproc::stream::out : reproc::stream::err;
                auto [bytesRead, readEc] = process_.read(which, buffer, kBufferSize);
                if (readEc && readEc != reproc::error::broken_pipe) {
                    break;
                }
                const std::size_t n = readEc == reproc::error::broken_pipe ? 0 : bytesRead;
                if (which == reproc::stream::out) {
                    outSink(reproc::stream::out, buffer, n);
                } else {
                    errSink(reproc::stream::err, buffer, n);
                }
            }
            outSink.Flush();
            errSink.Flush();
            // The child is gone, so nothing more can be written to it. Anything still
            // queued is completed with a broken pipe rather than left blocking its caller
            // forever -- this thread is the only one that could ever have serviced it.
            FailPendingStdinWork();

            auto [status, ec] = process_.wait(reproc::infinite);

            ProcessResult result;
            result.exitCode = ec ? -1 : status;
            result.wasTerminated = terminateRequested_.load() || killRequested_.load();

            std::lock_guard<std::mutex> lock(resultMutex_);
            cachedResult_ = result;
            resultCv_.notify_all();
        });
    }

    ~RealProcess() override {
        // Best-effort cleanup: a caller that drops the IProcess without ever calling
        // Wait()/WaitFor() must not leak a running child or hang the destructor forever.
        // Route through the same pendingAction_ flag the drain thread polls for -- do NOT
        // call process_.kill() from this thread, see the constructor comment above.
        if (IsRunning()) {
            killRequested_.store(true);
            pendingAction_.store(2);
        }
        if (drainThread_.joinable()) {
            drainThread_.join();
        }
#ifdef _WIN32
        // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE means this alone terminates any descendant
        // still alive, even if Kill() was never explicitly called.
        if (jobObject_ != nullptr) {
            CloseHandle(jobObject_);
            jobObject_ = nullptr;
        }
#endif
    }

    RealProcess(const RealProcess&) = delete;
    RealProcess& operator=(const RealProcess&) = delete;

    void WriteLine(const std::string& line) override {
        const std::error_code ec = SubmitStdinWork(line + "\n", /*close=*/false);
        // A broken pipe means the child is simply gone, which is a normal outcome (it
        // exited, or was cancelled) and not something to throw about.
        if (ec && ec != std::errc::broken_pipe) {
            throw mediatool::errors::MediaToolException(mediatool::errors::ErrorInfo::Make(
                "E_PROCESS_WRITE_FAILED", mediatool::errors::ErrorCategory::EngineFailure,
                "Failed to write to child process stdin", ec.message()));
        }
    }

    // Queued behind any pending writes rather than performed immediately, so "write the
    // command, then signal end of input" cannot close the pipe before the command has
    // gone through it.
    void CloseStdin() override { (void)SubmitStdinWork(std::string(), /*close=*/true); }

    ProcessResult Wait() override {
        std::unique_lock<std::mutex> lock(resultMutex_);
        resultCv_.wait(lock, [this] { return cachedResult_.has_value(); });
        return *cachedResult_;
    }

    std::optional<ProcessResult> WaitFor(int timeoutMs) override {
        std::unique_lock<std::mutex> lock(resultMutex_);
        const bool ready = resultCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                               [this] { return cachedResult_.has_value(); });
        return ready ? cachedResult_ : std::nullopt;
    }

    void Terminate() override {
        terminateRequested_.store(true);
        pendingAction_.store(1);
    }

    void Kill() override {
        killRequested_.store(true);
        pendingAction_.store(2);
#ifdef _WIN32
        // Unlike reproc's own kill/terminate (which only this drain thread may call, see
        // the constructor comment), TerminateJobObject() acts on a separate Win32 handle
        // (jobObject_) with no such restriction -- safe to call immediately from whatever
        // thread calls Kill(), and it takes the whole descendant tree down at once rather
        // than leaving that to reproc's single-process kill().
        if (jobObject_ != nullptr) {
            TerminateJobObject(jobObject_, 1);
        }
#endif
    }

    bool IsRunning() const override {
        std::lock_guard<std::mutex> lock(resultMutex_);
        return !cachedResult_.has_value();
    }

private:
    // One unit of stdin work for the drain thread to perform on the caller's behalf. The
    // caller blocks on `done` so WriteLine keeps its synchronous "wrote it, or threw"
    // contract even though the write itself happens on another thread.
    struct StdinWork {
        std::string data;
        bool close = false;
        bool done = false;
        std::error_code ec;
    };

    std::error_code SubmitStdinWork(std::string data, bool close) {
        auto work = std::make_shared<StdinWork>();
        work->data = std::move(data);
        work->close = close;

        std::unique_lock<std::mutex> lock(stdinMutex_);
        if (drainFinished_) {
            return std::make_error_code(std::errc::broken_pipe);
        }
        stdinQueue_.push_back(work);
        // Up to one poll interval, which is why that interval is short. Not signalled
        // directly: the drain thread may be inside poll(), and interrupting that safely
        // would need a self-pipe -- more machinery than a 50ms wait is worth.
        stdinCv_.wait(lock, [&work] { return work->done; });
        return work->ec;
    }

    // Drain thread only.
    void RunPendingStdinWork() {
        for (;;) {
            std::shared_ptr<StdinWork> work;
            {
                std::lock_guard<std::mutex> lock(stdinMutex_);
                if (stdinQueue_.empty()) return;
                work = stdinQueue_.front();
                stdinQueue_.pop_front();
            }

            std::error_code ec;
            if (work->close) {
                ec = process_.close(reproc::stream::in);
            } else {
                // Looped, because reproc::process::write() reports what it managed to
                // write rather than writing everything: the previous code discarded that
                // count, so a partial write silently truncated the line. A 4 MB request
                // line is exactly when that happens.
                std::size_t offset = 0;
                while (offset < work->data.size()) {
                    auto [written, writeEc] = process_.write(
                        reinterpret_cast<const uint8_t*>(work->data.data()) + offset,
                        work->data.size() - offset);
                    if (writeEc) {
                        ec = writeEc;
                        break;
                    }
                    if (written == 0) break;  // no progress; stop rather than spin
                    offset += written;
                }
            }

            {
                std::lock_guard<std::mutex> lock(stdinMutex_);
                work->ec = ec;
                work->done = true;
            }
            stdinCv_.notify_all();
        }
    }

    // Drain thread only, once it will never service the queue again.
    void FailPendingStdinWork() {
        std::deque<std::shared_ptr<StdinWork>> abandoned;
        {
            std::lock_guard<std::mutex> lock(stdinMutex_);
            drainFinished_ = true;
            abandoned.swap(stdinQueue_);
            for (auto& work : abandoned) {
                work->ec = std::make_error_code(std::errc::broken_pipe);
                work->done = true;
            }
        }
        stdinCv_.notify_all();
    }

    // How long the drain thread blocks in poll() before looking at its own inbox. It
    // bounds three things: how quickly Terminate()/Kill() take effect, how quickly a
    // queued stdin write goes out, and (inversely) how often this thread wakes for
    // nothing. It was 200ms when the loop only had to notice a stop request; a queued
    // write made it a latency floor for every write, which the IPC integration harness
    // measured directly as a 50ms round trip at the previous value. 10ms is 100 wakeups
    // per second per live child -- a poll() with a timeout and two atomic reads -- against
    // at most a handful of children.
    //
    // Production barely notices either way: the only C++ caller that writes to a child
    // writes one command line and closes stdin. The core's OWN stdin comes from the Rust
    // bridge, not from here, so nothing a user waits on pays this.
    static constexpr int kPollIntervalMs = 10;

    reproc::process process_;
    std::mutex stdinMutex_;
    std::condition_variable stdinCv_;
    std::deque<std::shared_ptr<StdinWork>> stdinQueue_;
    bool drainFinished_ = false;
    std::thread drainThread_;
    mutable std::mutex resultMutex_;
    std::condition_variable resultCv_;
    std::optional<ProcessResult> cachedResult_;
    std::atomic<bool> terminateRequested_{false};
    std::atomic<bool> killRequested_{false};
    // 0 = none, 1 = terminate, 2 = kill. Only ever consumed (exchanged back to 0) by the
    // drain thread, which is the only thread allowed to act on `process_`.
    std::atomic<int> pendingAction_{0};
#ifdef _WIN32
    HANDLE jobObject_ = nullptr;  // see the constructor comment for #9 (orphaned children)
#endif
};

}  // namespace

std::unique_ptr<IProcess> RealProcessRunner::Start(const std::string& executable,
                                                    const std::vector<std::string>& args,
                                                    const ProcessOptions& options,
                                                    OutputLineCallback onStdout,
                                                    OutputLineCallback onStderr) {
    std::vector<std::string> argv;
    argv.reserve(args.size() + 1);
    argv.push_back(executable);
    argv.insert(argv.end(), args.begin(), args.end());

    reproc::options reprocOptions;

    // Must outlive the call to start() -- reproc copies the pointer, not the string.
    std::string workingDirectory = options.workingDirectory;
    if (!workingDirectory.empty()) {
        reprocOptions.working_directory = workingDirectory.c_str();
    }

    if (!options.environment.empty()) {
        reprocOptions.env.behavior = reproc::env::extend;
        reprocOptions.env.extra = options.environment;
    }

    reproc::process process;
    std::error_code ec = process.start(argv, reprocOptions);
    if (ec) {
        throw mediatool::errors::MediaToolException(mediatool::errors::ErrorInfo::Make(
            "E_PROCESS_LAUNCH_FAILED", mediatool::errors::ErrorCategory::EngineFailure,
            "Failed to launch process: " + executable, ec.message()));
    }

    return std::make_unique<RealProcess>(std::move(process), std::move(onStdout), std::move(onStderr));
}

}  // namespace mediatool::process
