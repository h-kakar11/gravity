#pragma once

// std::filesystem-backed IFileSystem implementation -- the disk-touching side of the
// mockable interface (spec section 37).

#include "core/filesystem/IFileSystem.h"

namespace mediatool::filesystem {

class LocalFileSystem final : public IFileSystem {
public:
    bool Exists(const std::string& path) const override;

    // Populates path/filename/extension/category/sizeBytes/mimeType from the
    // filesystem itself. Media-specific fields (duration, codecs, fps, ...) are left
    // std::nullopt -- that's IMediaEngine::Probe's job via ffprobe, not this class's.
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
};

}  // namespace mediatool::filesystem
