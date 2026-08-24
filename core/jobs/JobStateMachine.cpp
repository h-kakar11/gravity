#include "core/jobs/JobStateMachine.h"

namespace mediatool::jobs {

bool CanTransition(JobState from, JobState to) {
    switch (from) {
        case JobState::Queued:
            return to == JobState::Starting || to == JobState::Cancelled ||
                   to == JobState::Waiting || to == JobState::Skipped;
        case JobState::Waiting:
            return to == JobState::Queued || to == JobState::Cancelled ||
                   to == JobState::Skipped;
        case JobState::Starting:
            return to == JobState::Running || to == JobState::Failed ||
                   to == JobState::Cancelled;
        case JobState::Running:
            return to == JobState::Paused || to == JobState::Completed ||
                   to == JobState::Failed || to == JobState::Cancelled;
        case JobState::Paused:
            return to == JobState::Running || to == JobState::Cancelled;
        case JobState::RetryWait:
            return to == JobState::Retrying || to == JobState::Cancelled;
        case JobState::Failed:
            return to == JobState::Retrying || to == JobState::RetryWait;
        case JobState::Skipped:
            return to == JobState::Waiting;
        case JobState::Retrying:
            return to == JobState::Running || to == JobState::Failed ||
                   to == JobState::Cancelled;
        case JobState::Completed:
        case JobState::Cancelled:
            return false;
    }
    return false;
}

}  // namespace mediatool::jobs
