#pragma once

// Scripted, no-real-process IProcessRunner for unit tests (spec section 37). Start()
// feeds the canned stdout/stderr lines straight to the caller's callbacks and completes
// with the canned exit code -- no child process is ever spawned.

#include <memory>
#include <string>
#include <vector>

#include "core/process/IProcessRunner.h"

namespace mediatool::process {

class MockProcessRunner : public IProcessRunner {
public:
    MockProcessRunner(std::vector<std::string> stdoutLines, std::vector<std::string> stderrLines,
                       int exitCode);

    std::unique_ptr<IProcess> Start(const std::string& executable,
                                     const std::vector<std::string>& args,
                                     const ProcessOptions& options,
                                     OutputLineCallback onStdout,
                                     OutputLineCallback onStderr) override;

    // Lines written via IProcess::WriteLine() on the process this runner most recently
    // created, in order. Backed by shared state so it stays valid after the IProcess
    // returned by Start() is destroyed.
    const std::vector<std::string>& WrittenLines() const;

private:
    std::vector<std::string> stdoutLines_;
    std::vector<std::string> stderrLines_;
    int exitCode_;
    std::shared_ptr<std::vector<std::string>> writtenLines_;
};

}  // namespace mediatool::process
