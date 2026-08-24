#pragma once

// Bounded exponential backoff for automatic retries (spec section 15).
//
// Pure arithmetic on purpose: it takes an attempt number and a RetryPolicy and returns a
// delay. It does not sleep, does not read a clock, and holds no state. The scheduler is
// what knows when a job becomes eligible again -- there are deliberately no scattered
// sleeps inside job code.
//
// Determinism matters here: the same attempt number always produces the same delay, so the
// retry timeline is exactly assertable in tests. There is no random jitter. Jitter exists
// to desynchronise many clients hammering one server; this is a single local desktop app
// retrying its own handful of jobs, so it would buy nothing and cost testability.

#include <cstdint>

#include "core/queue/QueueTypes.h"

namespace mediatool::queue {

// Delay before attempt number `attempt` (1 = the first *retry*, i.e. the second run
// overall). Grows as initialDelayMs * multiplier^(attempt-1), clamped to maxDelayMs.
// Returns 0 for attempt <= 0.
std::int64_t BackoffDelayMs(const RetryPolicy& policy, int attempt);

}  // namespace mediatool::queue
