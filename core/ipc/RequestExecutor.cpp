#include "core/ipc/RequestExecutor.h"

#include <system_error>
#include <utility>

#include "core/logging/Logger.h"

namespace mediatool::ipc {

RequestExecutor::RequestExecutor(std::size_t threads, std::size_t maxQueuedTasks)
    : maxQueuedTasks_(maxQueuedTasks == 0 ? 1 : maxQueuedTasks) {
    const std::size_t requested = threads == 0 ? 1 : threads;
    workers_.reserve(requested);
    for (std::size_t i = 0; i < requested; ++i) {
        try {
            workers_.emplace_back([this] { WorkerLoop(); });
        } catch (const std::system_error& e) {
            // Same reasoning as JobManager's pool: never let a partially built vector of
            // joinable threads be destroyed by an exception leaving this constructor.
            logging::Log::Warning("RequestExecutor",
                                   "Could only start " + std::to_string(workers_.size()) + " of " +
                                       std::to_string(requested) + " request threads (" + e.what() +
                                       "); blocking commands will run inline instead");
            break;
        }
    }
}

RequestExecutor::~RequestExecutor() { Shutdown(); }

void RequestExecutor::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
        queue_.clear();  // see the header: unstarted work has no one left to answer
    }
    queueCv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

bool RequestExecutor::TrySubmit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || workers_.empty() || queue_.size() >= maxQueuedTasks_) return false;
        queue_.push_back(std::move(task));
    }
    queueCv_.notify_one();
    return true;
}

std::size_t RequestExecutor::PendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void RequestExecutor::WorkerLoop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            queueCv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;  // queue_ was cleared by Shutdown()
            task = std::move(queue_.front());
            queue_.pop_front();
        }

        // A task is a whole request: handler, response serialization, and the stdout
        // write. If one throws, this thread must keep serving -- the alternative is a pool
        // that silently shrinks to nothing over the life of the process, and eventually a
        // command that never gets an answer.
        try {
            task();
        } catch (const std::exception& e) {
            logging::Log::Error("RequestExecutor",
                                 std::string("Unhandled exception escaped a queued request: ") + e.what());
        } catch (...) {
            logging::Log::Error("RequestExecutor", "Unhandled non-exception value escaped a queued request");
        }
    }
}

}  // namespace mediatool::ipc
