#include "core/filesystem/OutputNameRegistry.h"

#include <utility>

#include "core/filesystem/FilenameSanitizer.h"
#include "core/filesystem/PathUtils.h"

namespace mediatool::filesystem {

// --- Reservation ------------------------------------------------------------------------

OutputNameRegistry::Reservation::Reservation(OutputNameRegistry* registry, std::string key,
                                             std::string value)
    : registry_(registry), key_(std::move(key)), value_(std::move(value)) {}

OutputNameRegistry::Reservation::~Reservation() {
    Release();
}

OutputNameRegistry::Reservation::Reservation(Reservation&& other) noexcept
    : registry_(other.registry_), key_(std::move(other.key_)), value_(std::move(other.value_)) {
    other.registry_ = nullptr;
}

OutputNameRegistry::Reservation& OutputNameRegistry::Reservation::operator=(
    Reservation&& other) noexcept {
    if (this != &other) {
        Release();
        registry_ = other.registry_;
        key_ = std::move(other.key_);
        value_ = std::move(other.value_);
        other.registry_ = nullptr;
    }
    return *this;
}

void OutputNameRegistry::Reservation::Release() {
    if (registry_ == nullptr) return;
    registry_->ReleaseKey(key_);
    registry_ = nullptr;
}

// --- OutputNameRegistry -------------------------------------------------------------------

OutputNameRegistry& OutputNameRegistry::Instance() {
    static OutputNameRegistry instance;
    return instance;
}

void OutputNameRegistry::ReleaseKey(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    reserved_.erase(key);
}

void OutputNameRegistry::ClearForTesting() {
    std::lock_guard<std::mutex> lock(mutex_);
    reserved_.clear();
}

OutputNameRegistry::Reservation OutputNameRegistry::ReserveFilename(const std::string& desiredPath,
                                                                     const IFileSystem& fileSystem) {
    // The lock spans choosing AND recording the name. Splitting them would reintroduce
    // exactly the race this class exists to close: two callers could both settle on the
    // same free candidate before either had recorded it.
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string chosen = DeduplicateFilename(
        desiredPath, fileSystem,
        [this](const std::string& candidate) { return reserved_.count(candidate) > 0; });
    reserved_.insert(chosen);
    return Reservation(this, chosen, chosen);
}

OutputNameRegistry::Reservation OutputNameRegistry::ReserveBaseName(
    const std::string& directory, const std::string& desiredBaseName,
    const IFileSystem& fileSystem) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Base names are keyed with their directory so the same stem in two different folders
    // does not collide -- "Holiday" in Videos and "Holiday" in Music are unrelated.
    const std::string chosen = DeduplicateBaseName(
        directory, desiredBaseName, fileSystem, [this, &directory](const std::string& candidate) {
            return reserved_.count(paths::Join(directory, candidate)) > 0;
        });
    const std::string key = paths::Join(directory, chosen);
    reserved_.insert(key);
    return Reservation(this, key, chosen);
}

}  // namespace mediatool::filesystem
