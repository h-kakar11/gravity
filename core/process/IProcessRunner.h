#pragma once

// One of the five mockable interfaces called out in spec section 37, and the ONLY way
// anything in this codebase launches a child process. FFmpeg, ffprobe, and the Python
// downloader are all invoked through an IProcessRunner -- never via std::system(),
// _popen(), or hand-built shell command strings. `args` is always a structured argv
// vector; string-concatenating a command line is exactly the bug this interface exists
// to prevent (spec section 16).

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mediatool::process {

struct ProcessOptions {
    std::string workingDirectory;  // empty = inherit current working directory
    std::vector<std::pair<std::string, std::string>> environment;  // added to/overrides inherited env
};

struct ProcessResult {
    int exitCode = -1;
    bool wasTerminated = false;  // true if Terminate()/Kill() caused the exit
};

// Called once per line of output (split on '\n', line does not include the newline).
// Implementations must buffer partial lines internally -- a callback firing mid-line is
// a bug, not a valid interpretation of "line-buffered".
using OutputLineCallback = std::function<void(const std::string& line)>;

// A running or finished child process, returned by IProcessRunner::Start(). Never
// constructed directly.
class IProcess {
public:
    virtual ~IProcess() = default;

    virtual void WriteLine(const std::string& line) = 0;  // writes line + '\n' to child stdin
    virtual void CloseStdin() = 0;

    virtual ProcessResult Wait() = 0;  // blocks until the process exits
    virtual std::optional<ProcessResult> WaitFor(int timeoutMs) = 0;  // nullopt = still running

    virtual void Terminate() = 0;  // cooperative shutdown request
    virtual void Kill() = 0;       // forceful termination

    virtual bool IsRunning() const = 0;
};

class IProcessRunner {
public:
    virtual ~IProcessRunner() = default;

    // Starts `executable` with `args`. `onStdout`/`onStderr` may be called from a
    // background thread owned by the returned IProcess for as long as it is alive.
    virtual std::unique_ptr<IProcess> Start(const std::string& executable,
                                             const std::vector<std::string>& args,
                                             const ProcessOptions& options,
                                             OutputLineCallback onStdout,
                                             OutputLineCallback onStderr) = 0;
};

}  // namespace mediatool::process
