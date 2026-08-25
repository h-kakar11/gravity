#include "core/filesystem/FilenameReservationRegistry.h"

#include <filesystem>
#include <utility>

#include "core/errors/ErrorInfo.h"
#include "core/errors/MediaToolException.h"

namespace stdfs = std::filesystem;

namespace mediatool::filesystem {

namespace {

// Mirrors FilenameSanitizer.cpp's own bound on how many "(N)" suffixes to try before
// giving up -- see that file's comment for why this exists (a buggy IFileSystem that
// always reports a name as taken must not turn into an infinite loop).
constexpr int kMaxDeduplicationAttempts = 10000;

bool AnyFileHasBaseName(const std::vector<std::string>& names, const std::string& baseName) {
    for (const auto& name : names) {
        if (stdfs::path(name).stem().string() == baseName) return true;
    }
    return false;
}

}  // namespace

FilenameReservationRegistry::Reservation::Reservation(FilenameReservationRegistry* registry,
                                                        std::string directory, std::string baseName)
    : registry_(registry), directory_(std::move(directory)), baseName_(std::move(baseName)) {}

FilenameReservationRegistry::Reservation::~Reservation() { Release(); }

FilenameReservationRegistry::Reservation::Reservation(Reservation&& other) noexcept
    : registry_(other.registry_),
      directory_(std::move(other.directory_)),
      baseName_(std::move(other.baseName_)) {
    other.registry_ = nullptr;
}

FilenameReservationRegistry::Reservation& FilenameReservationRegistry::Reservation::operator=(
    Reservation&& other) noexcept {
    if (this != &other) {
        Release();
        registry_ = other.registry_;
        directory_ = std::move(other.directory_);
        baseName_ = std::move(other.baseName_);
        other.registry_ = nullptr;
    }
    return *this;
}

void FilenameReservationRegistry::Reservation::Release() {
    if (registry_ != nullptr) {
        registry_->ReleaseClaim(directory_, baseName_);
        registry_ = nullptr;
    }
}

FilenameReservationRegistry::Reservation FilenameReservationRegistry::Reserve(
    const std::string& directory, const std::string& desiredBaseName, const IFileSystem& fs) {
    // Listed once per call, not once per candidate: an output directory's contents don't
    // change between our own candidate attempts within this single call (nothing else in
    // this process can create files there without going through this same registry, and
    // cross-process collisions are out of scope, same as DeduplicateBaseName today).
    const std::vector<std::string> existing = fs.ListDirectory(directory);

    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i <= kMaxDeduplicationAttempts; ++i) {
        const std::string candidate =
            i == 0 ? desiredBaseName : desiredBaseName + " (" + std::to_string(i) + ")";
        const auto key = std::make_pair(directory, candidate);
        if (claimed_.count(key)) continue;
        if (AnyFileHasBaseName(existing, candidate)) continue;

        claimed_.insert(key);
        return Reservation(this, directory, candidate);
    }

    throw errors::MediaToolException(errors::ErrorInfo::Make(
        "E_DEDUP_EXHAUSTED", errors::ErrorCategory::Unknown,
        "Could not find a free base filename for " + desiredBaseName,
        "Exceeded " + std::to_string(kMaxDeduplicationAttempts) + " numbered variants in " + directory));
}

void FilenameReservationRegistry::ReleaseClaim(const std::string& directory, const std::string& baseName) {
    std::lock_guard<std::mutex> lock(mutex_);
    claimed_.erase(std::make_pair(directory, baseName));
}

}  // namespace mediatool::filesystem
