#pragma once

// The single authority for which JobState transitions are legal (docs/ipc-contract.md,
// spec sections 4-6). Job and JobManager must route every state change through
// CanTransition() rather than re-deriving the table -- this is the one place it's
// allowed to be spelled out.

#include "core/jobs/JobTypes.h"

namespace mediatool::jobs {

// True iff `from -> to` is a legal transition per the table below. Every self-transition
// is rejected, including e.g. RUNNING -> RUNNING. COMPLETED and CANCELLED have no valid
// outgoing transitions at all.
//
//   QUEUED    -> STARTING, CANCELLED
//   STARTING  -> RUNNING, FAILED, CANCELLED
//   RUNNING   -> PAUSED, COMPLETED, FAILED, CANCELLED
//   PAUSED    -> RUNNING, CANCELLED
//   FAILED    -> RETRYING
//   RETRYING  -> RUNNING, FAILED
bool CanTransition(JobState from, JobState to);

// The outcome of an attempted transition. Every transition entry point on Job returns one
// of these rather than throwing, because "the transition did not happen" is a normal,
// expected outcome under concurrency -- a worker thread calling MarkStarting() on a job a
// user cancelled microseconds earlier has lost a race, not hit a bug -- and an exception
// thrown from a worker thread for a routine race is how the #4 process abort happened in
// the first place. Callers that genuinely need "this must not have happened" (e.g. an IPC
// handler validating a user request) turn a non-Success result into an error themselves,
// where they have the context to describe it.
enum class TransitionResult {
    // The job moved into the requested state; side effects (timestamps, callbacks) ran.
    Success,
    // The job was already in the requested state. Idempotent no-op, not an error: the
    // caller's intent already holds.
    AlreadyInState,
    // The job had already reached a *different* terminal state (COMPLETED/FAILED/
    // CANCELLED) before this call. The caller lost a race with a cancellation or another
    // finalizer; the terminal state that got there first stands.
    AlreadyTerminal,
    // The requested transition is not legal from the job's current, non-terminal state.
    // This one really does indicate a caller bug (or a state machine misunderstanding)
    // and is worth logging where it is observed.
    InvalidTransition,
};

// Stable, human-readable spelling for logs and test failure messages.
const char* ToString(TransitionResult result);

}  // namespace mediatool::jobs
