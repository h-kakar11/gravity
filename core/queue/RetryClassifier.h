#pragma once

// Decides whether a failure is worth trying again (spec section 14).
//
// The governing rule is *default to not retrying*. A retry that can never succeed costs
// the user time, burns a retry budget, and buries the real error under a pile of identical
// ones. So classification is allow-list shaped: an error is Transient only when we can
// point at a specific reason to believe a second attempt might go differently. Everything
// else -- including anything unrecognized -- is Permanent.
//
// Classification looks at the structured ErrorInfo (category first, then the machine
// `code`), never at free-text stderr. Matching on prose would silently break the moment
// yt-dlp or ffmpeg reworded a message.

#include <string>

#include "core/errors/ErrorInfo.h"

namespace mediatool::queue {

enum class RetryDisposition {
    // Plausibly caused by something outside the request itself (a flaky network, a
    // rate-limited server, a temporarily busy resource). Worth a bounded retry.
    Transient,
    // Deterministically caused by the request or the environment. Retrying reproduces it
    // exactly, so the job fails immediately and permanently.
    Permanent,
};

std::string ToWireString(RetryDisposition disposition);

// The classification decision for `error`, together with why -- the reason string is
// surfaced in logs and in the job detail panel so a user can see *why* something is or is
// not being retried, rather than having to guess.
struct RetryClassification {
    RetryDisposition disposition = RetryDisposition::Permanent;
    std::string reason;

    bool IsTransient() const { return disposition == RetryDisposition::Transient; }
};

RetryClassification ClassifyRetry(const errors::ErrorInfo& error);

}  // namespace mediatool::queue
