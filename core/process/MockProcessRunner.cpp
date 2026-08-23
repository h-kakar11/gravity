#include "core/process/MockProcessRunner.h"

#include <utility>

namespace mediatool::process {

namespace {

class MockProcess : public IProcess {
public:
    MockProcess(const std::vector<std::string>& stdoutLines, const std::vector<std::string>& stderrLines,
                int exitCode, OutputLineCallback onStdout, OutputLineCallback onStderr,
                std::shared_ptr<std::vector<std::string>> writtenLines)
        : result_{exitCode, false}, writtenLines_(std::move(writtenLines)) {
        // Delivered synchronously from Start() -- IProcessRunner only requires callbacks
        // may run on a background thread, not that they must.
        for (const auto& line : stdoutLines) {
            if (onStdout) onStdout(line);
        }
        for (const auto& line : stderrLines) {
            if (onStderr) onStderr(line);
        }
    }

    void WriteLine(const std::string& line) override { writtenLines_->push_back(line); }
    void CloseStdin() override {}

    ProcessResult Wait() override {
        running_ = false;
        return result_;
    }

    std::optional<ProcessResult> WaitFor(int /*timeoutMs*/) override {
        running_ = false;
        return result_;
    }

    void Terminate() override {
        result_.wasTerminated = true;
        running_ = false;
    }

    void Kill() override {
        result_.wasTerminated = true;
        running_ = false;
    }

    bool IsRunning() const override { return running_; }

private:
    ProcessResult result_;
    bool running_ = true;
    std::shared_ptr<std::vector<std::string>> writtenLines_;
};

}  // namespace

MockProcessRunner::MockProcessRunner(std::vector<std::string> stdoutLines,
                                      std::vector<std::string> stderrLines, int exitCode)
    : stdoutLines_(std::move(stdoutLines)),
      stderrLines_(std::move(stderrLines)),
      exitCode_(exitCode),
      writtenLines_(std::make_shared<std::vector<std::string>>()) {}

std::unique_ptr<IProcess> MockProcessRunner::Start(const std::string& /*executable*/,
                                                    const std::vector<std::string>& /*args*/,
                                                    const ProcessOptions& /*options*/,
                                                    OutputLineCallback onStdout,
                                                    OutputLineCallback onStderr) {
    writtenLines_->clear();
    return std::make_unique<MockProcess>(stdoutLines_, stderrLines_, exitCode_, std::move(onStdout),
                                          std::move(onStderr), writtenLines_);
}

const std::vector<std::string>& MockProcessRunner::WrittenLines() const { return *writtenLines_; }

}  // namespace mediatool::process
