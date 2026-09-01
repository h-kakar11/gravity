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
            // RUNNING -> RETRYING, without passing through FAILED, is what makes an
            // automatic retry a continuation of one job rather than a terminal event
            // followed by a resurrection. Routing it through FAILED would tell the
            // scheduler this job had ended -- cancelling every job that depends on it --
            // and record a failure in Session History, both for an attempt the very next
            // scheduling decision is about to repeat. A retry that runs out of attempts
            // still goes RUNNING -> FAILED, which is the only terminal failure there is.
            return to == JobState::Paused || to == JobState::Completed ||
                   to == JobState::Failed || to == JobState::Cancelled ||
                   to == JobState::Retrying;
        case JobState::Paused:
            return to == JobState::Running || to == JobState::Cancelled;
        case JobState::Failed:
            return to == JobState::Retrying;
        case JobState::Retrying:
            // Cancelled too: a job parked on a backoff timer is exactly when a user is
            // most likely to give up on it, and it must not have to wait out the timer to
            // do so.
            return to == JobState::Running || to == JobState::Failed ||
                   to == JobState::Cancelled;
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
