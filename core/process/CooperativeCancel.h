#pragma once

// Shared cooperative cancel-then-kill polling loop for every long-running subprocess this
// codebase manages (ffmpeg, yt-dlp): FFmpegEngine::RunFfmpegJob and
// YtDlpProvider::RunPythonCommand each independently hand-rolled the identical loop
// (poll WaitFor(200ms); on cancellation, Terminate(), give it up to 2s, Kill() if it's
// still not gone) before this existed -- one implementation to get right and test, not
// two that can silently drift apart.

#include <functional>
#include <optional>

#include "core/process/IProcessRunner.h"

namespace mediatool::process {

struct WaitOutcome {
    // Populated when the process exited on its own (or was already exited by the time
    // this returned) without ever observing isCancelled() return true.
    std::optional<ProcessResult> result;
    // True if isCancelled() returned true at some point -- the process has already been
    // torn down (Terminate(), then Kill() if it didn't exit within the grace period) by
    // the time this returns. Callers throw their own specific cancellation error; this
    // helper doesn't know what error code/category/message each caller wants.
    bool wasCancelled = false;
};

// Polls `process` to completion, tearing it down cooperatively if `isCancelled` ever
// returns true. `isCancelled` may be empty (never cancellable) or absent-checked the same
// way every existing caller already did (`if (isCancelled && isCancelled())`).
WaitOutcome WaitForCompletionOrCancel(IProcess& process, const std::function<bool()>& isCancelled);

}  // namespace mediatool::process
