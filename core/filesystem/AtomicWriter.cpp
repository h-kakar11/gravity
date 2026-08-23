#include "core/filesystem/AtomicWriter.h"

#include <filesystem>
#include <system_error>
#include <utility>

#include "core/errors/ErrorInfo.h"
#include "core/errors/MediaToolException.h"

namespace stdfs = std::filesystem;

namespace mediatool::filesystem {

namespace {

std::string MakeTemporaryPath(const std::string& finalPath) {
    const stdfs::path p(finalPath);
    const std::string ext = p.extension().string();   // includes leading dot, or empty
    const std::string stem = p.stem().string();
    return (p.parent_path() / (stem + ".processing" + ext)).string();
}

}  // namespace

AtomicWriter::AtomicWriter(std::string finalPath)
    : finalPath_(std::move(finalPath)), temporaryPath_(MakeTemporaryPath(finalPath_)) {}

AtomicWriter::~AtomicWriter() {
    if (committed_ || temporaryPath_.empty()) {
        return;
    }
    std::error_code ec;
    stdfs::remove(temporaryPath_, ec);  // best-effort: destructors must not throw
}

void AtomicWriter::MoveFrom(AtomicWriter& other) noexcept {
    finalPath_ = std::move(other.finalPath_);
    temporaryPath_ = std::move(other.temporaryPath_);
    committed_ = other.committed_;
    other.temporaryPath_.clear();
    other.committed_ = true;  // moved-from object no longer owns the temp file
}

AtomicWriter::AtomicWriter(AtomicWriter&& other) noexcept {
    MoveFrom(other);
}

AtomicWriter& AtomicWriter::operator=(AtomicWriter&& other) noexcept {
    if (this != &other) {
        if (!committed_ && !temporaryPath_.empty()) {
            std::error_code ec;
            stdfs::remove(temporaryPath_, ec);
        }
        MoveFrom(other);
    }
    return *this;
}

void AtomicWriter::Commit() {
    if (committed_) {
        return;
    }

    std::error_code existsEc;
    if (!stdfs::exists(temporaryPath_, existsEc)) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_ATOMIC_WRITE_MISSING_TEMP", errors::ErrorCategory::EngineFailure,
            "Temporary output file was not found; nothing to commit.",
            "path=" + temporaryPath_));
    }

    // std::filesystem::rename atomically replaces an existing regular file at
    // finalPath_ on both POSIX (rename(2)) and this toolchain's Windows implementation
    // (MoveFileExW with MOVEFILE_REPLACE_EXISTING) -- no separate "delete old" step.
    std::error_code renameEc;
    stdfs::rename(temporaryPath_, finalPath_, renameEc);
    if (renameEc) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_ATOMIC_WRITE_RENAME_FAILED", errors::ErrorCategory::EngineFailure,
            "Could not finalize the output file.",
            "from=" + temporaryPath_ + " to=" + finalPath_ + " error=" + renameEc.message()));
    }
    committed_ = true;
}

}  // namespace mediatool::filesystem
