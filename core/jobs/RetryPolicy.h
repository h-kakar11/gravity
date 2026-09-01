#pragma once

// Whether a failed job should be tried again, and how long to wait first.
//
// Threadless and clockless on purpose, exactly like SchedulerCore: it is a pure function
// of an ErrorInfo, an attempt count and a policy. That keeps every interesting question
// -- "does a disk-full error retry?", "does the fifth attempt back off further than the
// fourth?" -- answerable without starting a thread or waiting on a real timer, and leaves
// JobManager as the only class in this subsystem that touches concurrency.
//
// The decision is deliberately made of two independent signals, because either one alone
// gets a class of failures wrong:
//
//   * ErrorInfo::recoverable, which the layer that produced the failure sets. It is the
//     only thing that knows a "network error" was a DNS lookup that will fail identically
//     forever versus a 503 that will not.
//   * The error's category, used as a VETO. A category that is deterministic by nature --
//     the file is not there, the disk is full, the format is unsupported -- is never
//     retried no matter what the flag says, because a provider that mislabels one of
//     those would otherwise turn a clear failure into three of them, several seconds
//     apart, with the same message.
//
// So a retry needs both: the producer said it might work, and the failure is not of a
// kind that cannot.

#include <chrono>
#include <string>

#include "core/errors/ErrorInfo.h"

namespace mediatool::jobs {

struct RetryPolicy {
    // TOTAL attempts, including the first. 1 disables automatic retry entirely, which is
    // why it is the floor rather than 0 -- "zero attempts" would describe a job that never
    // runs.
    int maxAttempts = 3;
    // Wait before attempt 2. Long enough that a transient network condition has actually
    // had a chance to change, short enough not to feel like a hang.
    std::chrono::milliseconds initialBackoff{2000};
    double backoffMultiplier = 2.0;
    // Ceiling, so a policy with a large maxAttempts cannot schedule a retry an hour out.
    std::chrono::milliseconds maxBackoff{60000};
};

struct RetryDecision {
    bool shouldRetry = false;
    // How long to wait before the next attempt. Zero when shouldRetry is false.
    std::chrono::milliseconds delay{0};
    // Human-readable, for the log line and the job's status message. Always populated --
    // "why did this not retry" is the question that gets asked, and answering it from the
    // decision itself beats reconstructing it from the error afterwards.
    std::string reason;
};

// True if this KIND of failure can ever be worth retrying, ignoring attempt counts. See
// the header comment for why this is separate from ErrorInfo::recoverable rather than
// derived from it.
bool IsRetryableError(const errors::ErrorInfo& error);

// `attemptsSoFar` counts attempts that have already run, including the one that just
// failed -- so it is 1 the first time a job fails.
RetryDecision DecideRetry(const errors::ErrorInfo& error, int attemptsSoFar,
                           const RetryPolicy& policy);

}  // namespace mediatool::jobs
