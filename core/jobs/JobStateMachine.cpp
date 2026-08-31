#include "core/jobs/JobStateMachine.h"

namespace mediatool::jobs {

bool CanTransition(JobState from, JobState to) {
    switch (from) {
        case JobState::Queued:
            return to == JobState::Starting || to == JobState::Cancelled;
        case JobState::Starting:
            return to == JobState::Running || to == JobState::Failed ||
                   to == JobState::Cancelled;
        case JobState::Running:
            return to == JobState::Paused || to == JobState::Completed ||
                   to == JobState::Failed || to == JobState::Cancelled;
        case JobState::Paused:
            return to == JobState::Running || to == JobState::Cancelled;
        case JobState::Failed:
            return to == JobState::Retrying;
        case JobState::Retrying:
            return to == JobState::Running || to == JobState::Failed;
        case JobState::Completed:
        case JobState::Cancelled:
            return false;
    }
    return false;
}

const char* ToString(TransitionResult result) {
    switch (result) {
        case TransitionResult::Success:
            return "Success";
        case TransitionResult::AlreadyInState:
            return "AlreadyInState";
        case TransitionResult::AlreadyTerminal:
            return "AlreadyTerminal";
        case TransitionResult::InvalidTransition:
            return "InvalidTransition";
    }
    return "Unknown";
}

}  // namespace mediatool::jobs
