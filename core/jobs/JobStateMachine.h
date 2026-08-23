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

}  // namespace mediatool::jobs
