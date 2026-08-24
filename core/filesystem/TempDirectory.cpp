#include "core/filesystem/TempDirectory.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <utility>

#include "core/errors/ErrorInfo.h"
#include "core/errors/MediaToolException.h"

namespace stdfs = std::filesystem;

namespace mediatool::filesystem {

namespace {

std::string ResolveBaseDir(const std::optional<std::string>& overrideBaseDir) {
    if (overrideBaseDir) {
        return *overrideBaseDir;
    }
    if (const char* envOverride = std::getenv("MEDIATOOL_TEMP_BASE_DIR")) {
        return envOverride;
    }
    if (const char* localAppData = std::getenv("LOCALAPPDATA")) {
        return localAppData;
    }
    // No LOCALAPPDATA (non-Windows dev/CI environment) -- fall back under the CWD.
    return (stdfs::current_path() / ".mediatool-temp").string();
}

std::string LeafDirName(const std::string& jobId) {
    return jobId.rfind("job-", 0) == 0 ? jobId : "job-" + jobId;
}

}  // namespace

TempDirectory::TempDirectory(const std::string& jobId,
                              std::optional<std::string> overrideBaseDir) {
    const stdfs::path dir =
        stdfs::path(ResolveBaseDir(overrideBaseDir)) / "Gravity" / "temp" / LeafDirName(jobId);

    std::error_code ec;
    stdfs::create_directories(dir, ec);
    if (ec) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_TEMP_DIR_CREATE_FAILED", errors::ErrorCategory::PermissionError,
            "Could not create temporary working directory.",
            "path=" + dir.string() + " error=" + ec.message()));
    }
    path_ = dir.string();
}

TempDirectory::~TempDirectory() {
    if (released_ || path_.empty()) {
        return;
    }
    std::error_code ec;
    stdfs::remove_all(path_, ec);  // best-effort: destructors must not throw
}

void TempDirectory::MoveFrom(TempDirectory& other) noexcept {
    path_ = std::move(other.path_);
    released_ = other.released_;
    other.path_.clear();
    other.released_ = true;  // moved-from object no longer owns the directory
}

TempDirectory::TempDirectory(TempDirectory&& other) noexcept {
    MoveFrom(other);
}

TempDirectory& TempDirectory::operator=(TempDirectory&& other) noexcept {
    if (this != &other) {
        if (!released_ && !path_.empty()) {
            std::error_code ec;
            stdfs::remove_all(path_, ec);
        }
        MoveFrom(other);
    }
    return *this;
}

void TempDirectory::Release() {
    released_ = true;
}

}  // namespace mediatool::filesystem
