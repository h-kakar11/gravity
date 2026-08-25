#include "core/process/RealProcessRunner.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

#include <reproc++/reproc.hpp>

#include "core/errors/ErrorInfo.h"
#include "core/errors/MediaToolException.h"

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

                auto [events, pollEc] =
                    process_.poll(reproc::event::out | reproc::event::err, reproc::milliseconds(200));
                if (pollEc == std::errc::timed_out) {
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
    }

    RealProcess(const RealProcess&) = delete;
    RealProcess& operator=(const RealProcess&) = delete;

    void WriteLine(const std::string& line) override {
        std::lock_guard<std::mutex> lock(writeMutex_);
        const std::string data = line + "\n";
        auto [written, ec] = process_.write(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        (void)written;
        if (ec && ec != std::errc::broken_pipe) {
            throw mediatool::errors::MediaToolException(mediatool::errors::ErrorInfo::Make(
                "E_PROCESS_WRITE_FAILED", mediatool::errors::ErrorCategory::EngineFailure,
                "Failed to write to child process stdin", ec.message()));
        }
    }

    void CloseStdin() override { process_.close(reproc::stream::in); }

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
    }

    bool IsRunning() const override {
        std::lock_guard<std::mutex> lock(resultMutex_);
        return !cachedResult_.has_value();
    }

private:
    reproc::process process_;
    std::thread drainThread_;
    std::mutex writeMutex_;
    mutable std::mutex resultMutex_;
    std::condition_variable resultCv_;
    std::optional<ProcessResult> cachedResult_;
    std::atomic<bool> terminateRequested_{false};
    std::atomic<bool> killRequested_{false};
    // 0 = none, 1 = terminate, 2 = kill. Only ever consumed (exchanged back to 0) by the
    // drain thread, which is the only thread allowed to act on `process_`.
    std::atomic<int> pendingAction_{0};
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
