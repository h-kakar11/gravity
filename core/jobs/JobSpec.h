#pragma once

// A job as a *recipe* rather than a *status report*.
//
// JobManager::JobSnapshot answers "how is this job doing" -- state, progress, result,
// timestamps. It deliberately does not carry the URL, the input path, the output
// directory or the processing options, because nothing that reads a snapshot needs them.
// That makes a snapshot useless for the one thing crash recovery has to do: build the job
// again from nothing after the process that owned it died.
//
// So a JobSpec is the createJob request that produced a job, kept verbatim, plus the
// identity and scheduling facts the request itself did not carry (the generated id, the
// creation time) and the two facts only a *run* can know: where on disk this job started
// writing, and how many times it has already been recovered.
//
// It is deliberately the raw request params rather than a per-type struct: the params are
// already validated by the createJob handlers, already versioned by
// docs/ipc-contract.md, and replaying them through those same handlers is what makes a
// recovered job identical to a fresh one instead of a second, subtly different
// construction path that can drift.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/jobs/JobTypes.h"

namespace mediatool::jobs {

// Where a run had started writing, once it knows. Not known at submission -- a download's
// filename comes from the video title and a conversion's from the input stem, both
// resolved on the worker thread -- so this is filled in mid-run and is absent for a job
// that died before reserving a name (which is also exactly the case with nothing to clean
// up).
struct JobArtifactLocation {
    std::string outputDirectory;
    std::string filenameBase;
};

struct JobSpec {
    JobId id;
    JobType type = JobType::Test;
    // The `params` object of the createJob request that produced this job, verbatim.
    nlohmann::json params = nlohmann::json::object();
    std::string createdAt;
    std::optional<JobArtifactLocation> artifact;
    // How many times this job has already been rebuilt after a crash. A job that crashes
    // the core on every run would otherwise re-queue itself forever on every launch --
    // see kMaxRecoveryAttempts.
    int recoveryCount = 0;

    nlohmann::json ToJson() const;
    // Throws nlohmann::json::exception on a malformed entry -- callers that read a file
    // written by an older or corrupt build must catch it and skip the entry rather than
    // failing the whole load (see InProgressJobStore::Load).
    static JobSpec FromJson(const nlohmann::json& json);
};

// After this many failed recoveries a spec is dropped instead of re-queued. Three is
// enough to survive an unlucky crash or two and low enough that a job which reliably
// takes the process down stops taking it down at every launch.
inline constexpr int kMaxRecoveryAttempts = 3;

}  // namespace mediatool::jobs
