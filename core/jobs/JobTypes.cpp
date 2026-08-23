#include "core/jobs/JobTypes.h"

#include <stdexcept>

namespace mediatool::jobs {

std::string ToWireString(JobType type) {
    switch (type) {
        case JobType::Download: return "DOWNLOAD";
        case JobType::Conversion: return "CONVERSION";
        case JobType::Compression: return "COMPRESSION";
        case JobType::Batch: return "BATCH";
        case JobType::Workflow: return "WORKFLOW";
        case JobType::Test: return "TEST";
    }
    throw std::invalid_argument("Unrecognized JobType enum value");
}

JobType JobTypeFromWireString(const std::string& wire) {
    if (wire == "DOWNLOAD") return JobType::Download;
    if (wire == "CONVERSION") return JobType::Conversion;
    if (wire == "COMPRESSION") return JobType::Compression;
    if (wire == "BATCH") return JobType::Batch;
    if (wire == "WORKFLOW") return JobType::Workflow;
    if (wire == "TEST") return JobType::Test;
    throw std::invalid_argument("Unrecognized JobType wire string: " + wire);
}

std::string ToWireString(JobState state) {
    switch (state) {
        case JobState::Queued: return "QUEUED";
        case JobState::Starting: return "STARTING";
        case JobState::Running: return "RUNNING";
        case JobState::Paused: return "PAUSED";
        case JobState::Completed: return "COMPLETED";
        case JobState::Failed: return "FAILED";
        case JobState::Cancelled: return "CANCELLED";
        case JobState::Retrying: return "RETRYING";
    }
    throw std::invalid_argument("Unrecognized JobState enum value");
}

JobState JobStateFromWireString(const std::string& wire) {
    if (wire == "QUEUED") return JobState::Queued;
    if (wire == "STARTING") return JobState::Starting;
    if (wire == "RUNNING") return JobState::Running;
    if (wire == "PAUSED") return JobState::Paused;
    if (wire == "COMPLETED") return JobState::Completed;
    if (wire == "FAILED") return JobState::Failed;
    if (wire == "CANCELLED") return JobState::Cancelled;
    if (wire == "RETRYING") return JobState::Retrying;
    throw std::invalid_argument("Unrecognized JobState wire string: " + wire);
}

bool IsActiveState(JobState state) {
    switch (state) {
        case JobState::Queued:
        case JobState::Starting:
        case JobState::Running:
        case JobState::Paused:
        case JobState::Retrying:
            return true;
        case JobState::Completed:
        case JobState::Failed:
        case JobState::Cancelled:
            return false;
    }
    return false;
}

bool IsTerminalState(JobState state) {
    switch (state) {
        case JobState::Completed:
        case JobState::Failed:
        case JobState::Cancelled:
            return true;
        default:
            return false;
    }
}

}  // namespace mediatool::jobs
