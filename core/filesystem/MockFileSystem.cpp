#include "core/filesystem/MockFileSystem.h"

#include "core/errors/MediaToolException.h"
#include "core/filesystem/PathUtils.h"

namespace mediatool::filesystem {

bool MockFileSystem::Exists(const std::string& path) const {
    return files_.count(path) > 0 || directories_.count(path) > 0;
}

FileInfo MockFileSystem::Inspect(const std::string& path) const {
    const auto it = files_.find(path);
    if (it == files_.end()) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_FILE_NOT_FOUND", errors::ErrorCategory::FileNotFound, "File not found: " + path));
    }
    return it->second;
}

void MockFileSystem::Copy(const std::string& from, const std::string& to) {
    const auto it = files_.find(from);
    if (it == files_.end()) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_FILE_NOT_FOUND", errors::ErrorCategory::FileNotFound, "File not found: " + from));
    }
    FileInfo copy = it->second;
    copy.path = to;
    copy.filename = paths::GetFilename(to);
    files_[to] = copy;
}

void MockFileSystem::Move(const std::string& from, const std::string& to) {
    Copy(from, to);
    files_.erase(from);
}

void MockFileSystem::Rename(const std::string& path, const std::string& newName) {
    const std::string target = paths::Join(paths::GetParentDirectory(path), newName);
    Move(path, target);
}

void MockFileSystem::Delete(const std::string& path) {
    files_.erase(path);
    directories_.erase(path);
    deletedPaths_.push_back(path);
}

void MockFileSystem::CreateDirectory(const std::string& path) {
    directories_.insert(path);
    createdDirectories_.push_back(path);
}

std::uint64_t MockFileSystem::CalculateSize(const std::string& path) const {
    const auto it = files_.find(path);
    return it == files_.end() ? 0 : it->second.sizeBytes;
}

std::string MockFileSystem::GetExtension(const std::string& path) const {
    return paths::GetExtension(path);
}

std::string MockFileSystem::GetFilename(const std::string& path) const {
    return paths::GetFilename(path);
}

std::string MockFileSystem::GetParentDirectory(const std::string& path) const {
    return paths::GetParentDirectory(path);
}

std::optional<std::uint64_t> MockFileSystem::GetAvailableDiskSpace(const std::string& path) const {
    const auto it = diskSpaceByPath_.find(path);
    if (it != diskSpaceByPath_.end()) return it->second;
    return std::nullopt;
}

std::vector<std::string> MockFileSystem::ListDirectory(const std::string& directory) const {
    std::vector<std::string> names;
    for (const auto& [path, info] : files_) {
        if (paths::GetParentDirectory(path) == directory) {
            names.push_back(info.filename.empty() ? paths::GetFilename(path) : info.filename);
        }
    }
    return names;
}

void MockFileSystem::AddFile(FileInfo info) {
    directories_.insert(paths::GetParentDirectory(info.path));
    files_[info.path] = std::move(info);
}

void MockFileSystem::AddDirectory(const std::string& path) { directories_.insert(path); }

void MockFileSystem::SetAvailableDiskSpace(const std::string& path, std::uint64_t bytes) {
    diskSpaceByPath_[path] = bytes;
}

}  // namespace mediatool::filesystem
