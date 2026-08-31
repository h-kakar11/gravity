#include "core/process/CooperativeCancel.h"

namespace mediatool::process {

WaitOutcome WaitForCompletionOrCancel(IProcess& process, const std::function<bool()>& isCancelled) {
    WaitOutcome outcome;
    while (true) {
        if (isCancelled && isCancelled()) {
            process.Terminate();
            if (auto terminated = process.WaitFor(2000)) {
                outcome.result = *terminated;
            } else {
                process.Kill();
                outcome.result = process.Wait();
            }
            outcome.wasCancelled = true;
            return outcome;
        }
        if (auto finishedResult = process.WaitFor(200)) {
            outcome.result = *finishedResult;
            return outcome;
        }
    }
}

}  // namespace mediatool::process
