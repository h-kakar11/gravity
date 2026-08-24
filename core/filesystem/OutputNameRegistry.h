#pragma once

// Stops two concurrently running jobs from choosing the same output filename.
//
// Deduplication on its own cannot prevent this. It answers "is this name free?" by looking
// at the disk, and two jobs that ask at the same moment -- before either has written
// anything -- both get "yes" and both pick it. With Phase 1's concurrency of one that could
// never happen; with Phase 5 running several jobs at once it happens readily, and the
// result is silent data loss: three downloads of the same title all report success and one
// file exists.
//
// So a job does not merely pick a name, it *reserves* one. A reservation is held for as
// long as the job might still write to that path and is released automatically when the
// Reservation object goes out of scope -- including when the job throws, is cancelled, or
// fails, which is exactly when a manually-released registry would leak.
//
// Scope is deliberately process-wide, because the thing being protected is process-wide:
// the set of names this application's own jobs are about to write. It does not coordinate
// with other processes, and does not need to -- the app does not support two instances
// writing to one folder, and a name written by something else is caught by the ordinary
// on-disk check.

#include <mutex>
#include <set>
#include <string>

#include "core/filesystem/IFileSystem.h"

namespace mediatool::filesystem {

class OutputNameRegistry {
public:
    // Holds a claim on one name. Move-only, and releases on destruction.
    class Reservation {
    public:
        Reservation() = default;
        ~Reservation();

        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        Reservation(Reservation&& other) noexcept;
        Reservation& operator=(Reservation&& other) noexcept;

        // The name that was actually reserved -- a full path for ReserveFilename, a base
        // name for ReserveBaseName. Empty for a default-constructed reservation.
        const std::string& Value() const { return value_; }
        bool IsHeld() const { return registry_ != nullptr; }

        // Gives up the claim early. Safe to call more than once. A job calls this once its
        // output is committed and the name is defended by the file actually existing.
        void Release();

    private:
        friend class OutputNameRegistry;
        Reservation(OutputNameRegistry* registry, std::string key, std::string value);

        OutputNameRegistry* registry_ = nullptr;
        std::string key_;
        std::string value_;
    };

    // The registry every job shares. A singleton because the resource it guards -- the
    // names about to appear in the user's folders -- is itself global to the process.
    static OutputNameRegistry& Instance();

    // Reserves a collision-free variant of `desiredPath`, treating both files on disk and
    // names other jobs currently hold as taken. Throws (E_DEDUP_EXHAUSTED) if no free
    // variant is found.
    Reservation ReserveFilename(const std::string& desiredPath, const IFileSystem& fileSystem);

    // Same, for a base name with no extension yet -- the case where an external tool picks
    // the container after the fact (see DeduplicateBaseName).
    Reservation ReserveBaseName(const std::string& directory, const std::string& desiredBaseName,
                                const IFileSystem& fileSystem);

    // Drops every outstanding reservation. Only for tests, which share one process and
    // would otherwise leak claims between cases.
    void ClearForTesting();

private:
    friend class Reservation;
    void ReleaseKey(const std::string& key);

    std::mutex mutex_;
    std::set<std::string> reserved_;
};

}  // namespace mediatool::filesystem
