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
    Queued,
    Starting,
    Running,
    Paused,
    Completed,
    Failed,
    Cancelled,
    Retrying,
};

std::string ToWireString(JobState state);
JobState JobStateFromWireString(const std::string& wire);

// True if a job in `state` is still active (occupies a JobManager execution slot).
bool IsActiveState(JobState state);

// True if `state` is terminal (Completed, Failed, Cancelled) -- no further transitions
// except FAILED -> RETRYING are valid from a terminal state per the state machine.
bool IsTerminalState(JobState state);

}  // namespace mediatool::jobs
