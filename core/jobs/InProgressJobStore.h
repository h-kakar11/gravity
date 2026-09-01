#pragma once

// The set of jobs that have not finished yet, on disk, so a crash does not lose them.
//
// This is the deliberate counterpart to JobHistoryStore, which persists *terminal*
// snapshots and whose header states that persisting in-flight work is a non-goal. That
// was true while a job's only description was a status snapshot, which cannot rebuild
// anything. With JobSpec (core/jobs/JobSpec.h) it can, so this store keeps one spec per
// unfinished job and drops it the moment the job reaches a terminal state.
//
// What "survives a crash" means here, precisely, because the weaker claim is the honest
// one: a killed job is REBUILT AND RE-RUN FROM THE START, not resumed. Its ffmpeg or
// yt-dlp child died with the process, its partial output is not a checkpoint, and there
// is no protocol on either side for resuming one. What is recovered is the intent -- this
// URL, to this directory, at this quality -- which is what a user actually loses today
// when the app dies mid-queue with twenty things pending.
//
// Same durability pattern as JobHistoryStore and JsonFileSettingsStore: write a unique
// sibling temp file then rename, tolerate a corrupt or absent file by starting empty, and
// never throw -- every method here runs on a path (job submission, job completion) that
// must not be disrupted by a persistence failure.

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/jobs/JobSpec.h"
#include "core/jobs/JobTypes.h"

namespace mediatool::jobs {

// "%LOCALAPPDATA%\Gravity\in_progress_jobs.json". Reads LOCALAPPDATA from the
// environment; never throws.
std::string DefaultInProgressJobsFilePath();

class InProgressJobStore {
public:
    explicit InProgressJobStore(std::string filePath);

    // Submission order, oldest first -- the order a recovery pass must resubmit in for
    // `dependsOn` edges to still point backwards. Returns empty (logging a warning, never
    // throwing) if the file is absent, unreadable or corrupt; an individual entry that
    // fails to parse is skipped rather than discarding the whole file, since one bad
    // record should not cost a user the other nineteen.
    std::vector<JobSpec> Load() const;

    // Inserts `spec`, or replaces the existing entry with the same id in place (keeping
    // its position, so submission order is preserved across updates).
    void Put(const JobSpec& spec);

    // Records where a run started writing, so a later recovery pass can delete what a
    // killed run left behind. A no-op if `id` is unknown -- a job that has already
    // finished is not one anything needs to clean up after.
    void SetArtifactLocation(const JobId& id, const JobArtifactLocation& artifact);

    // Records how many attempts `id` has spent, so the retry budget survives a restart.
    // A no-op if `id` is unknown, same as SetArtifactLocation.
    void SetAttemptCount(const JobId& id, int attempts);

    // Drops `id`. Called when a job reaches a terminal state: from that moment there is
    // nothing to recover, and leaving the entry would re-run finished work on next launch.
    void Remove(const JobId& id);

    // Empties the store. Used after a recovery pass has consumed it, so the specs that
    // pass re-submitted are re-added by the submission path itself rather than lingering
    // as duplicates of jobs that now exist.
    void Clear();

private:
    // Every method is a read-modify-write of one file, and Put/Remove are called from
    // JobManager worker threads via the job state-changed callback. Serializing the whole
    // read-modify-write is what makes "the store matches the live job set" true rather
    // than probable -- exactly the interleaving JobHistoryStore documents.
    mutable std::mutex mutex_;
    std::string filePath_;

    std::vector<JobSpec> LoadLocked() const;
    void SaveLocked(const std::vector<JobSpec>& specs) const;
};

}  // namespace mediatool::jobs
