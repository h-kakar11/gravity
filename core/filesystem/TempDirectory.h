#pragma once

// Per-job isolated scratch space (spec section 12): equivalent to
// %LOCALAPPDATA%\Gravity\temp\job-<id>\. RAII-owned so a job's temp files are
// guaranteed cleaned up on any exit path (success, failure, exception) unless the job
// explicitly promotes its output out first via Release().

#include <optional>
#include <string>

namespace mediatool::filesystem {

class TempDirectory {
public:
    // `jobId` becomes the leaf directory name (prefixed with "job-" if not already).
    // `overrideBaseDir`, when set, replaces the %LOCALAPPDATA% lookup -- this is the
    // hook tests use so they never touch the real user profile. When unset, the
    // MEDIATOOL_TEMP_BASE_DIR environment variable is checked next (same purpose, for
    // callers that can't inject a constructor argument), then %LOCALAPPDATA%, then a
    // ".gravity-temp" directory under the current working directory as a last resort.
    //
    // Throws errors::MediaToolException (ErrorCategory::PermissionError) if the
    // directory can't be created.
    explicit TempDirectory(const std::string& jobId,
                            std::optional<std::string> overrideBaseDir = std::nullopt);
    ~TempDirectory();

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    TempDirectory(TempDirectory&& other) noexcept;
    TempDirectory& operator=(TempDirectory&& other) noexcept;

    const std::string& Path() const { return path_; }

    // Prevents the destructor from deleting the directory (e.g. after promoting a
    // finished job's output out of it). Idempotent.
    void Release();
    bool IsReleased() const { return released_; }

private:
    std::string path_;
    bool released_ = false;

    void MoveFrom(TempDirectory& other) noexcept;
};

}  // namespace mediatool::filesystem
