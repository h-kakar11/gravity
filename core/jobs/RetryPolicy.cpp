#include "core/jobs/RetryPolicy.h"

#include <algorithm>
#include <cmath>

namespace mediatool::jobs {

namespace {

// Categories where a second attempt cannot plausibly do better than the first, whatever
// the producer set `recoverable` to. Each one names a condition the machine is in, not a
// condition of the moment: the file is still not there, the disk is still full, ffmpeg
// still cannot read that container, the user still asked to stop.
bool IsPermanentCategory(errors::ErrorCategory category) {
    switch (category) {
        case errors::ErrorCategory::FileNotFound:
        case errors::ErrorCategory::InvalidFile:
        case errors::ErrorCategory::UnsupportedFormat:
        case errors::ErrorCategory::PermissionError:
        case errors::ErrorCategory::DiskSpaceError:
        case errors::ErrorCategory::Cancelled:
            return true;
        case errors::ErrorCategory::EngineFailure:
        case errors::ErrorCategory::DownloadFailure:
        case errors::ErrorCategory::NetworkError:
        case errors::ErrorCategory::Unknown:
            return false;
    }
    return false;
}

}  // namespace

bool IsRetryableError(const errors::ErrorInfo& error) {
    if (IsPermanentCategory(error.category)) {
        return false;
    }
    return error.recoverable;
}

RetryDecision DecideRetry(const errors::ErrorInfo& error, int attemptsSoFar,
                           const RetryPolicy& policy) {
    RetryDecision decision;

    if (!IsRetryableError(error)) {
        decision.reason = "the failure (" + error.code + ") is not the kind that a retry can fix";
        return decision;
    }
    if (attemptsSoFar >= policy.maxAttempts) {
        decision.reason = "already tried " + std::to_string(attemptsSoFar) + " time(s), the limit";
        return decision;
    }

    // Exponential from the first retry: attemptsSoFar == 1 waits initialBackoff,
    // attemptsSoFar == 2 waits initialBackoff * multiplier, and so on -- computed in
    // double and clamped, so a large multiplier or attempt count overflows into the
    // ceiling rather than into a negative duration.
    const double exponent = static_cast<double>(std::max(0, attemptsSoFar - 1));
    const double scaled = static_cast<double>(policy.initialBackoff.count()) *
                           std::pow(std::max(1.0, policy.backoffMultiplier), exponent);
    const double capped = std::min(scaled, static_cast<double>(policy.maxBackoff.count()));

    decision.shouldRetry = true;
    decision.delay = std::chrono::milliseconds(static_cast<long long>(capped));
    decision.reason = "attempt " + std::to_string(attemptsSoFar + 1) + " of " +
                       std::to_string(policy.maxAttempts) + " after a recoverable " + error.code;
    return decision;
}

}  // namespace mediatool::jobs
