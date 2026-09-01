#pragma once

// Drives a REAL mediatool-core subprocess over its real stdio NDJSON protocol.
//
// Everything else in the test tree stops at a class boundary: the job tests use a mock
// engine, the provider tests use a fake process runner, the validation tests call the
// validators directly. All of that is worth having and none of it can catch the failures
// that live BETWEEN the pieces -- a handler registered under the wrong name, a response
// written without its id, a malformed line that kills the read loop, an event emitted in
// the wrong order. Those only appear when something actually speaks the protocol.
//
// The child is spawned through the same process::RealProcessRunner the app uses, so the
// spawn path itself is under test too.
//
// Isolation: LOCALAPPDATA is redirected into a per-test temporary directory, so the
// settings file, job history and in-progress-job store the child reads and writes are
// never the developer's real ones. MEDIATOOL_PYTHON_PATH / MEDIATOOL_DOWNLOADER_SCRIPT
// are pointed at real existing files so the downloader-availability gate (issue #79)
// passes and the validation behind it is reachable.

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/process/IProcessRunner.h"
#include "core/process/RealProcessRunner.h"

namespace mediatool::integration {

// One line the core wrote to stdout: either a response (has "id") or an event (has
// "event").
struct CoreLine {
    nlohmann::json json;
    bool IsResponse() const { return json.contains("id"); }
    bool IsEvent() const { return json.contains("event"); }
    std::string EventName() const { return json.value("event", std::string()); }
};

class CoreProcess {
public:
    // `corePath` is the built mediatool-core executable (CMake passes its location as a
    // compile definition). Throws std::runtime_error if it cannot be started.
    explicit CoreProcess(const std::string& corePath);
    ~CoreProcess();

    CoreProcess(const CoreProcess&) = delete;
    CoreProcess& operator=(const CoreProcess&) = delete;

    // Sends one request and blocks until the response with that id arrives, or the
    // timeout expires (in which case the returned optional is empty -- deliberately not
    // an exception, so a test can assert "no response" as an outcome).
    std::optional<nlohmann::json> Send(const std::string& command, const nlohmann::json& params,
                                        std::chrono::milliseconds timeout =
                                            std::chrono::seconds(10));

    // Writes a raw line to the child's stdin, bypassing request construction entirely --
    // for testing what the read loop does with input that is not a well-formed request.
    void SendRawLine(const std::string& line);

    // Blocks until an event named `eventName` arrives for `jobId` (any job if `jobId` is
    // empty), or the timeout expires.
    std::optional<CoreLine> WaitForEvent(const std::string& eventName, const std::string& jobId,
                                          std::chrono::milliseconds timeout =
                                              std::chrono::seconds(10));

    // Every event seen so far, oldest first.
    std::vector<CoreLine> EventsSoFar() const;

    // Closes stdin and waits for the child to exit. Returns its exit code.
    int Shutdown();

    const std::filesystem::path& LocalAppData() const { return localAppData_; }

private:
    void OnStdoutLine(const std::string& line);

    std::filesystem::path tempRoot_;
    std::filesystem::path localAppData_;
    process::RealProcessRunner runner_;
    std::unique_ptr<process::IProcess> child_;
    bool stdinClosed_ = false;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<CoreLine> responses_;
    std::vector<CoreLine> events_;
    std::vector<std::string> stderrLines_;
    int nextRequestId_ = 1;
};

}  // namespace mediatool::integration
