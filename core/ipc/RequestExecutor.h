#pragma once

// A small bounded thread pool for IPC commands that block (audit #8).
//
// The core's request loop is a single thread: read a line, run the handler, write the
// response, read the next line. That is the right shape for handlers that take
// microseconds, and the wrong one for handlers that wait on a network round trip through a
// yt-dlp subprocess. `inspectDownloadUrl` takes 1-3 seconds against a healthy site and, on
// an unresponsive one, up to its 30-second deadline -- and for that entire time the loop
// is not reading stdin, so a cancelJob the user just clicked, or a getJob the queue screen
// needs, sits unread in the pipe. Bounding how long a stall lasts (which the deadline
// already does) is not the same as not stalling.
//
// So: the loop hands blocking commands to this executor and immediately goes back to
// reading. Nothing about the wire protocol changes -- responses are correlated by `id` and
// docs/ipc-contract.md has always said requests may complete out of order -- and the
// stdout writer is already serialized (see WriteLine in app/core/main.cpp), so a response
// written from a pool thread is indistinguishable from one written by the loop.
//
// Both bounds are deliberate. The thread count caps how many subprocesses a burst of
// inspects can spawn at once; the queue depth caps how much work a client can pile up,
// because "queue it and hope" is how an unbounded queue turns a busy period into an
// out-of-memory kill.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace mediatool::ipc {

class RequestExecutor {
public:
    // Starts `threads` workers (0 is treated as 1) accepting at most `maxQueuedTasks`
    // waiting tasks. As with JobManager, a pool that cannot start every thread runs with
    // the ones it got rather than failing construction; unlike JobManager, a pool with no
    // threads at all is still usable -- see TrySubmit, whose caller must have a
    // run-it-here fallback anyway.
    RequestExecutor(std::size_t threads, std::size_t maxQueuedTasks);
    ~RequestExecutor();

    RequestExecutor(const RequestExecutor&) = delete;
    RequestExecutor& operator=(const RequestExecutor&) = delete;

    // Queues `task`. Returns false -- having done nothing -- if the queue is full, if the
    // pool has no workers, or if the executor is shutting down. The caller decides what
    // that means for its request; it must never silently drop it, because a request with
    // no response is a client hanging until its own timeout.
    bool TrySubmit(std::function<void()> task);

    // Stops accepting work, discards tasks that have not started, and joins the workers.
    // Queued-but-unstarted tasks are dropped rather than drained: Shutdown() runs when
    // stdin has closed, i.e. when the shell that sent those requests is gone and no answer
    // it could produce would reach anyone. Tasks already running are always waited for --
    // they may be holding a subprocess. Safe to call more than once; the destructor calls
    // it too.
    void Shutdown();

    std::size_t ThreadCount() const { return workers_.size(); }
    // Tasks queued but not yet started. Diagnostics only -- it is stale the moment it is
    // returned.
    std::size_t PendingCount() const;

private:
    void WorkerLoop();

    mutable std::mutex mutex_;
    std::condition_variable queueCv_;
    std::deque<std::function<void()>> queue_;
    bool stopping_ = false;
    const std::size_t maxQueuedTasks_;
    std::vector<std::thread> workers_;
};

}  // namespace mediatool::ipc
