#include "core/process/CooperativeCancel.h"

#include <gtest/gtest.h>

namespace mediatool::process {
namespace {

// A tiny scripted IProcess giving this test control over exactly when WaitFor() reports
// "still running" vs. finished, and whether the grace-period WaitFor(2000) after
// Terminate() succeeds or times out -- MockProcessRunner's MockProcess always completes
// synchronously on the first WaitFor(), which can't exercise either branch of
// WaitForCompletionOrCancel's cancel path (Terminate-succeeds vs. Terminate-times-out-so-Kill).
class ScriptedProcess : public IProcess {
public:
    explicit ScriptedProcess(int waitForCallsBeforeFinished, bool terminateSucceeds)
        : waitForCallsBeforeFinished_(waitForCallsBeforeFinished), terminateSucceeds_(terminateSucceeds) {}

    void WriteLine(const std::string&) override {}
    void CloseStdin() override {}

    ProcessResult Wait() override {
        killed_ = true;
        return ProcessResult{/*exitCode=*/-1, /*wasTerminated=*/true};
    }

    std::optional<ProcessResult> WaitFor(int /*timeoutMs*/) override {
        if (terminateCalled_) {
            // Post-Terminate() grace-period poll.
            if (terminateSucceeds_) return ProcessResult{/*exitCode=*/0, /*wasTerminated=*/true};
            return std::nullopt;  // times out -- caller should Kill()
        }
        ++waitForCalls_;
        if (waitForCalls_ < waitForCallsBeforeFinished_) return std::nullopt;
        return ProcessResult{/*exitCode=*/0, /*wasTerminated=*/false};
    }

    void Terminate() override { terminateCalled_ = true; }
    void Kill() override { killCalled_ = true; }
    bool IsRunning() const override { return waitForCalls_ < waitForCallsBeforeFinished_; }

    bool TerminateCalled() const { return terminateCalled_; }
    bool KillCalled() const { return killCalled_; }

private:
    int waitForCallsBeforeFinished_;
    bool terminateSucceeds_;
    int waitForCalls_ = 0;
    bool terminateCalled_ = false;
    bool killCalled_ = false;
    bool killed_ = false;
};

}  // namespace

TEST(CooperativeCancel, ReturnsResultWhenProcessFinishesWithoutCancellation) {
    ScriptedProcess process(/*waitForCallsBeforeFinished=*/2, /*terminateSucceeds=*/true);
    const WaitOutcome outcome = WaitForCompletionOrCancel(process, /*isCancelled=*/nullptr);

    EXPECT_FALSE(outcome.wasCancelled);
    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_EQ(outcome.result->exitCode, 0);
    EXPECT_FALSE(process.TerminateCalled());
    EXPECT_FALSE(process.KillCalled());
}

TEST(CooperativeCancel, TerminatesCleanlyWhenCancelledAndProcessExitsWithinGracePeriod) {
    ScriptedProcess process(/*waitForCallsBeforeFinished=*/1000, /*terminateSucceeds=*/true);
    const WaitOutcome outcome = WaitForCompletionOrCancel(process, [] { return true; });

    EXPECT_TRUE(outcome.wasCancelled);
    EXPECT_TRUE(process.TerminateCalled());
    EXPECT_FALSE(process.KillCalled());  // exited on its own within the grace period
}

TEST(CooperativeCancel, KillsWhenCancelledAndProcessDoesNotExitWithinGracePeriod) {
    ScriptedProcess process(/*waitForCallsBeforeFinished=*/1000, /*terminateSucceeds=*/false);
    const WaitOutcome outcome = WaitForCompletionOrCancel(process, [] { return true; });

    EXPECT_TRUE(outcome.wasCancelled);
    EXPECT_TRUE(process.TerminateCalled());
    EXPECT_TRUE(process.KillCalled());
}

}  // namespace mediatool::process
