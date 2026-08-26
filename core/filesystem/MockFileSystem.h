#pragma once

// Scripted, no-real-disk IFileSystem for unit tests (spec section 37). Backed by an
// in-memory map of path -> FileInfo, so tests never touch the real disk and can assert
// on exactly what a job/engine attempted (deleted paths, created directories).
// GetExtension/GetFilename/GetParentDirectory are pure string logic and reuse
// core/filesystem/PathUtils rather than re-deriving it here.

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/filesystem/IFileSystem.h"

namespace mediatool::filesystem {

class MockFileSystem : public IFileSystem {
public:
    bool Exists(const std::string& path) const override;
    FileInfo Inspect(const std::string& path) const override;
    void Copy(const std::string& from, const std::string& to) override;
    void Move(const std::string& from, const std::string& to) override;
    void Rename(const std::string& path, const std::string& newName) override;
    void Delete(const std::string& path) override;
    void DeleteFile(const std::string& path) override;
    void CreateDirectory(const std::string& path) override;
    std::uint64_t CalculateSize(const std::string& path) const override;
    std::string GetExtension(const std::string& path) const override;
    std::string GetFilename(const std::string& path) const override;
    std::string GetParentDirectory(const std::string& path) const override;
    std::optional<std::uint64_t> GetAvailableDiskSpace(const std::string& path) const override;
    std::vector<std::string> ListDirectory(const std::string& directory) const override;

    // --- test scripting ----------------------------------------------------------------
    // Registers a file at `info.path` with these contents. Its parent directory (derived
    // via PathUtils, same as LocalFileSystem would) is implicitly created if not already
    // known, so ListDirectory() on it returns this file without a separate AddDirectory().
    void AddFile(FileInfo info);
    void AddDirectory(const std::string& path);
    void SetAvailableDiskSpace(const std::string& path, std::uint64_t bytes);

    // --- test observation --------------------------------------------------------------
    const std::vector<std::string>& DeletedPaths() const { return deletedPaths_; }
    const std::vector<std::string>& CreatedDirectories() const { return createdDirectories_; }

private:
    std::unordered_map<std::string, FileInfo> files_;
    std::unordered_set<std::string> directories_;
    std::unordered_map<std::string, std::uint64_t> diskSpaceByPath_;
    std::vector<std::string> deletedPaths_;
    std::vector<std::string> createdDirectories_;
};

}  // namespace mediatool::filesystem
