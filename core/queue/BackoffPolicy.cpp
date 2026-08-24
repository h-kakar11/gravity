#include "core/queue/BackoffPolicy.h"

#include <algorithm>
#include <cmath>

namespace mediatool::queue {

std::int64_t BackoffDelayMs(const RetryPolicy& policy, int attempt) {
    if (attempt <= 0) return 0;

    // Computed in double and clamped rather than multiplied in int64: a large multiplier
    // and a high attempt number overflow an integer accumulator long before the clamp
    // would have caught it, and an overflowed delay is a scheduling bug that only shows up
    // on the unlucky run.
    const double growth = std::pow(policy.multiplier, static_cast<double>(attempt - 1));
    const double raw = static_cast<double>(policy.initialDelayMs) * growth;

    if (!std::isfinite(raw) || raw >= static_cast<double>(policy.maxDelayMs)) {
        return policy.maxDelayMs;
    }
    return std::max<std::int64_t>(0, static_cast<std::int64_t>(raw));
}

}  // namespace mediatool::queue
