#pragma once

// Real IProcessRunner backed by reproc++ (spec sections 16, 37). The concrete IProcess
// implementation (RealProcess) is an implementation detail kept entirely in the .cpp --
// callers only ever see it through the IProcess interface.

#include "core/process/IProcessRunner.h"

namespace mediatool::process {

class RealProcessRunner : public IProcessRunner {
public:
    std::unique_ptr<IProcess> Start(const std::string& executable,
                                     const std::vector<std::string>& args,
                                     const ProcessOptions& options,
                                     OutputLineCallback onStdout,
                                     OutputLineCallback onStderr) override;
};

}  // namespace mediatool::process
