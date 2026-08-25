#pragma once

// Fixes the TOCTOU race (#12) in DeduplicateBaseName-style filename allocation: probing
// "is this name free on disk?" and then later writing to it are two separate steps, so
// two jobs racing to allocate a name for the same title in the same output directory can
// both compute the same "next free" name and clobber each other once concurrency > 1 (a
// prerequisite for the "parallel processing" feature). This registry adds a process-wide,
// in-memory claim on top of the disk check so two concurrent callers can never both walk
// away with the same (directory, baseName) pair.
//
// One instance lives in AppContext and is shared by every job type that allocates an
// output filename (DownloadJob and, from Phase 2 on, MediaProcessingJob) -- reservations
// are released automatically when the returned Reservation guard is destroyed, normally
// when the owning job reaches a terminal state.

#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "core/filesystem/IFileSystem.h"

namespace mediatool::filesystem {

class FilenameReservationRegistry {
public:
    // RAII claim on one (directory, baseName) pair. Releases automatically on
    // destruction (or early via Release()); safe to move, not to copy. A default
    // -constructed Reservation holds no claim and releasing it is a no-op.
    class Reservation {
    public:
        Reservation() = default;
        ~Reservation();

        Reservation(Reservation&& other) noexcept;
        Reservation& operator=(Reservation&& other) noexcept;
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;

        // The claimed base name (may differ from the name originally requested, e.g.
        // "My Video (1)" instead of "My Video" if the plain name was already taken or
        // claimed by another in-flight reservation).
        const std::string& BaseName() const { return baseName_; }

        // Releases the claim early (e.g. once the job that reserved it is done and about
        // to write its output verification result). Idempotent; the destructor calls
        // this too, so an explicit prior call just makes it a no-op there.
        void Release();

    private:
        friend class FilenameReservationRegistry;
        Reservation(FilenameReservationRegistry* registry, std::string directory, std::string baseName);

        FilenameReservationRegistry* registry_ = nullptr;
        std::string directory_;
        std::string baseName_;
    };

    // Finds a base name in `directory` that is both free on disk (no existing file has
    // this exact stem, same notion DeduplicateBaseName uses) and not already claimed by
    // another live Reservation, claims it atomically with that check, and returns an RAII
    // guard for the claim. Throws errors::MediaToolException{Unknown, "E_DEDUP_EXHAUSTED",
    // ...} if no free/unclaimed variant is found within a bounded number of attempts,
    // mirroring DeduplicateBaseName's own exhaustion guard.
    Reservation Reserve(const std::string& directory, const std::string& desiredBaseName,
                        const IFileSystem& fs);

private:
    void ReleaseClaim(const std::string& directory, const std::string& baseName);

    std::mutex mutex_;
    std::set<std::pair<std::string, std::string>> claimed_;
};

}  // namespace mediatool::filesystem
