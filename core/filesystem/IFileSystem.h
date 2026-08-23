#pragma once

// One of the five mockable interfaces called out in spec section 37. Every filesystem
// touch in this codebase goes through IFileSystem so tests never hit the real disk and
// so path handling stays centralized (spec section 11: use std::filesystem internally,
// never manually concatenate paths).

#include <cstdint>
#include <optional>
#include <string>

#include "core/filesystem/FileInfo.h"

namespace mediatool::filesystem {

class IFileSystem {
public:
    virtual ~IFileSystem() = default;

    virtual bool Exists(const std::string& path) const = 0;

    // Throws errors::MediaToolException{ErrorCategory::FileNotFound, ...} if `path`
    // doesn't exist. Populates media-specific fields via ffprobe when the media engine
    // is available; leaves them std::nullopt otherwise (never throws for that reason).
    virtual FileInfo Inspect(const std::string& path) const = 0;

    virtual void Copy(const std::string& from, const std::string& to) = 0;
    virtual void Move(const std::string& from, const std::string& to) = 0;
    virtual void Rename(const std::string& path, const std::string& newName) = 0;
    virtual void Delete(const std::string& path) = 0;
    virtual void CreateDirectory(const std::string& path) = 0;

    virtual std::uint64_t CalculateSize(const std::string& path) const = 0;  // recursive for directories

    virtual std::string GetExtension(const std::string& path) const = 0;   // no leading dot, lowercase
    virtual std::string GetFilename(const std::string& path) const = 0;    // with extension, no directory
    virtual std::string GetParentDirectory(const std::string& path) const = 0;

    // nullopt if the path's drive/volume free space can't be determined.
    virtual std::optional<std::uint64_t> GetAvailableDiskSpace(const std::string& path) const = 0;
};

}  // namespace mediatool::filesystem
