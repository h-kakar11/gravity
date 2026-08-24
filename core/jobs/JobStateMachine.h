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
//   QUEUED     -> STARTING, CANCELLED, WAITING, SKIPPED
//   WAITING    -> QUEUED, CANCELLED, SKIPPED
//   STARTING   -> RUNNING, FAILED, CANCELLED
//   RUNNING    -> PAUSED, COMPLETED, FAILED, CANCELLED
//   PAUSED     -> RUNNING, CANCELLED
//   RETRY_WAIT -> RETRYING, CANCELLED
//   FAILED     -> RETRYING, RETRY_WAIT
//   SKIPPED    -> WAITING
//   RETRYING   -> RUNNING, FAILED, CANCELLED
//   COMPLETED  -> (none)
//   CANCELLED  -> (none)
//
// Notes on the less obvious edges:
//
//  * QUEUED -> WAITING covers a dependency that went un-terminal again underneath a job
//    that was already queued -- the user manually retried the dependency.
//  * QUEUED/WAITING -> SKIPPED is how a dependency failure propagates: the dependent never
//    ran, so FAILED would misreport whose fault it was.
//  * FAILED -> RETRY_WAIT is the automatic path (a transient error earned a backoff);
//    FAILED -> RETRYING is the manual "Retry" button, which skips the backoff entirely.
//  * SKIPPED -> WAITING is a manual retry of a skipped job: it re-enters dependency
//    evaluation rather than jumping straight to runnable.
//  * RETRYING -> CANCELLED exists because a retry that has been picked up but has not yet
//    reached RUNNING is still cancellable.
//
// PAUSING has deliberately not been added. Pause here is either cooperative and immediate
// (a job that checkpoints through Job::WaitWhilePaused) or unsupported for that job type --
// there is no asynchronous pause handshake that would need a state to sit in. See
// docs/phase-5.md "Pause semantics".
bool CanTransition(JobState from, JobState to);

}  // namespace mediatool::jobs
