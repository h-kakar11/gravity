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

void MockFileSystem::Rename(const std::string& path, const std::string& newName) {
    const std::string target = paths::Join(paths::GetParentDirectory(path), newName);
    const auto it = files_.find(path);
    if (it == files_.end()) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_FILE_NOT_FOUND", errors::ErrorCategory::FileNotFound, "File not found: " + path));
    }
    FileInfo renamed = it->second;
    renamed.path = target;
    renamed.filename = paths::GetFilename(target);
    files_[target] = renamed;
    files_.erase(path);
}

namespace {
bool IsWithin(const std::string& candidate, const std::string& directory) {
    return candidate.rfind(directory + "\\", 0) == 0 || candidate.rfind(directory + "/", 0) == 0;
}
}  // namespace

void MockFileSystem::Delete(const std::string& path) {
    // Mirrors LocalFileSystem::Delete's std::filesystem::remove_all semantics: removes
    // `path` itself and, if it is a directory, everything nested under it -- so tests
    // exercising cleanup-after-failure logic can actually observe the real-world
    // "deleted a whole directory tree" hazard rather than a mock that can't represent it.
    std::vector<std::string> nestedFiles;
    for (const auto& [filePath, info] : files_) {
        if (IsWithin(filePath, path)) nestedFiles.push_back(filePath);
    }
    for (const auto& filePath : nestedFiles) files_.erase(filePath);

    std::vector<std::string> nestedDirs;
    for (const auto& dirPath : directories_) {
        if (IsWithin(dirPath, path)) nestedDirs.push_back(dirPath);
    }
    for (const auto& dirPath : nestedDirs) directories_.erase(dirPath);

    files_.erase(path);
    directories_.erase(path);
    deletedPaths_.push_back(path);
}

void MockFileSystem::DeleteFile(const std::string& path) {
    if (directories_.count(path) > 0) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_DELETE_FILE_IS_DIRECTORY", errors::ErrorCategory::InvalidFile,
            "Refusing to recursively delete a directory via DeleteFile().", "path=" + path));
    }
    files_.erase(path);
    deletedPaths_.push_back(path);
}

void MockFileSystem::CreateDirectory(const std::string& path) {
    directories_.insert(path);
    createdDirectories_.push_back(path);
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
