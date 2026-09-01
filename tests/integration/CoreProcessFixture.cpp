#include "tests/integration/CoreProcessFixture.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <stdexcept>

namespace fs = std::filesystem;

namespace mediatool::integration {

namespace {

fs::path MakeUniqueTempRoot() {
    static std::atomic<unsigned long long> counter{0};
    const fs::path root = fs::temp_directory_path() /
                           ("mediatool_ipc_it_" + std::to_string(counter.fetch_add(1)) + "_" +
                            std::to_string(static_cast<unsigned long long>(
                                std::chrono::steady_clock::now().time_since_epoch().count())));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

}  // namespace

CoreProcess::CoreProcess(const std::string& corePath) {
    tempRoot_ = MakeUniqueTempRoot();
    localAppData_ = tempRoot_ / "AppData";
    std::error_code ec;
    fs::create_directories(localAppData_, ec);

    process::ProcessOptions options;
    // Redirects settings, job history and the in-progress-job store into this test's own
    // directory. Without it the child reads and rewrites the developer's real files.
    options.environment.emplace_back("LOCALAPPDATA", localAppData_.string());
    options.environment.emplace_back("MEDIATOOL_TEMP_BASE_DIR", (tempRoot_ / "temp").string());
    // Real, existing files so the downloader-availability gate passes and the validation
    // behind it is reachable. Nothing here ever actually runs them.
    options.environment.emplace_back("MEDIATOOL_PYTHON_PATH", MEDIATOOL_TEST_PYTHON);
    options.environment.emplace_back("MEDIATOOL_DOWNLOADER_SCRIPT", MEDIATOOL_TEST_DOWNLOADER);

    child_ = runner_.Start(
        corePath, {}, options, [this](const std::string& line) { OnStdoutLine(line); },
        [this](const std::string& line) {
            std::lock_guard<std::mutex> lock(mutex_);
            stderrLines_.push_back(line);
        });
    if (!child_) {
        throw std::runtime_error("could not start mediatool-core at " + corePath);
    }
}

CoreProcess::~CoreProcess() {
    if (child_) {
        if (!stdinClosed_) child_->CloseStdin();
        if (!child_->WaitFor(2000)) {
            child_->Kill();
            (void)child_->Wait();
        }
    }
    std::error_code ec;
    fs::remove_all(tempRoot_, ec);
}

void CoreProcess::OnStdoutLine(const std::string& line) {
    if (line.empty()) return;
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(line);
    } catch (const nlohmann::json::exception&) {
        // Every stdout line is supposed to be one JSON object -- the protocol reserves
        // stdout for exactly that (log output goes to stderr). A line that is not is a
        // protocol violation, and a test asserting on the stream should see it as such
        // rather than have it silently dropped here.
        std::lock_guard<std::mutex> lock(mutex_);
        stderrLines_.push_back("NON-JSON STDOUT LINE: " + line);
        return;
    }

    CoreLine coreLine{std::move(parsed)};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (coreLine.IsResponse()) {
            responses_.push_back(std::move(coreLine));
        } else {
            events_.push_back(std::move(coreLine));
        }
    }
    cv_.notify_all();
}

void CoreProcess::SendRawLine(const std::string& line) { child_->WriteLine(line); }

std::optional<nlohmann::json> CoreProcess::Send(const std::string& command,
                                                 const nlohmann::json& params,
                                                 std::chrono::milliseconds timeout) {
    std::string id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = "req-" + std::to_string(nextRequestId_++);
    }
    nlohmann::json request{{"id", id}, {"command", command}, {"params", params}};
    child_->WriteLine(request.dump());

    std::unique_lock<std::mutex> lock(mutex_);
    const bool arrived = cv_.wait_for(lock, timeout, [this, &id] {
        return std::any_of(responses_.begin(), responses_.end(), [&id](const CoreLine& line) {
            return line.json.value("id", std::string()) == id;
        });
    });
    if (!arrived) return std::nullopt;

    const auto it = std::find_if(responses_.begin(), responses_.end(), [&id](const CoreLine& line) {
        return line.json.value("id", std::string()) == id;
    });
    return it->json;
}

std::optional<CoreLine> CoreProcess::WaitForEvent(const std::string& eventName,
                                                   const std::string& jobId,
                                                   std::chrono::milliseconds timeout) {
    auto matches = [&](const CoreLine& line) {
        if (line.EventName() != eventName) return false;
        if (jobId.empty()) return true;
        return line.json.value("jobId", std::string()) == jobId;
    };

    std::unique_lock<std::mutex> lock(mutex_);
    const bool arrived = cv_.wait_for(lock, timeout, [this, &matches] {
        return std::any_of(events_.begin(), events_.end(), matches);
    });
    if (!arrived) return std::nullopt;
    return *std::find_if(events_.begin(), events_.end(), matches);
}

std::vector<CoreLine> CoreProcess::EventsSoFar() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
}

int CoreProcess::Shutdown() {
    if (!stdinClosed_) {
        child_->CloseStdin();
        stdinClosed_ = true;
    }
    // The read loop exits on end-of-stream, then AppContext is torn down -- which is
    // itself worth exercising, since that is where the worker pool is joined and the
    // stores are written.
    if (auto result = child_->WaitFor(10000)) {
        return result->exitCode;
    }
    child_->Kill();
    return child_->Wait().exitCode;
}

}  // namespace mediatool::integration
