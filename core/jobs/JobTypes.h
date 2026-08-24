#pragma once

// Shared job vocabulary. See docs/ipc-contract.md for the wire-format rules this header
// implements (UPPER_SNAKE_CASE enum wire strings). Every other module includes this
// header rather than redeclaring these types -- do not duplicate them.

#include <cstdint>
#include <string>

namespace mediatool::jobs {

// Opaque job identifier, format "job-<uuid4>". Never assume numeric or sortable.
using JobId = std::string;

// Generates a fresh, unique JobId ("job-<uuid4>"). Implemented in JobId.cpp.
JobId GenerateJobId();

enum class JobType {
    Download,
    Conversion,
    Compression,
    Batch,
    Workflow,
    // Phase-1-only synthetic job used to prove the pipeline end-to-end. Not a real
    // user-facing feature; excluded from the product's public capability list.
    Test,
};

// Serializes/parses the UPPER_SNAKE_CASE wire form defined in docs/ipc-contract.md.
// Throws std::invalid_argument on an unrecognized wire string.
std::string ToWireString(JobType type);
JobType JobTypeFromWireString(const std::string& wire);

enum class JobState {
    // Ready to run as soon as the scheduler has a free slot.
    Queued,
    // Held back because at least one dependency has not completed successfully yet.
    Waiting,
    Starting,
    Running,
    Paused,
    // An automatic retry has been scheduled; the job becomes eligible again once its
    // backoff delay elapses. See core/queue/BackoffPolicy.h.
    RetryWait,
    Completed,
    Failed,
    Cancelled,
    // A dependency failed or was cancelled, so this job will never run. Distinct from
    // Failed: nothing about this job itself went wrong.
    Skipped,
    Retrying,
};

std::string ToWireString(JobState state);
JobState JobStateFromWireString(const std::string& wire);

// True if a job in `state` is still live in the queue -- i.e. it may still run, whether
// it is currently executing, pending, blocked, or waiting out a retry backoff.
bool IsActiveState(JobState state);

// True if a job in `state` is occupying one of the scheduler's concurrency slots. This is
// narrower than IsActiveState: a Queued or RetryWait job is active but consumes no slot.
bool IsExecutingState(JobState state);

// True if `state` is terminal (Completed, Failed, Cancelled, Skipped). A terminal job is
// never scheduled again unless an explicit retry moves it out of the terminal state first
// (FAILED -> RETRYING/RETRY_WAIT, SKIPPED -> WAITING) -- see JobStateMachine.h.
bool IsTerminalState(JobState state);

}  // namespace mediatool::jobs
