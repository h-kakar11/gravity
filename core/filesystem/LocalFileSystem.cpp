#include "core/filesystem/LocalFileSystem.h"

#include <filesystem>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#include "core/errors/ErrorInfo.h"
#include "core/errors/MediaToolException.h"
#include "core/filesystem/PathUtils.h"

namespace stdfs = std::filesystem;

namespace mediatool::filesystem {

namespace {

// Extension -> category. Deliberately a local, best-effort map rather than a shared
// registry: nothing outside file inspection needs "is this extension a video" logic in
// Phase 1, and CapabilitiesFor() (FileInfo.h) already owns the category -> capability
// side of the vocabulary.
FileCategory CategoryFromExtension(const std::string& ext) {
    static const std::unordered_set<std::string> kVideo = {
        "mp4", "mkv", "avi", "mov", "wmv", "flv", "webm", "m4v", "mpg", "mpeg", "3gp", "ts", "m2ts",
    };
    static const std::unordered_set<std::string> kAudio = {
        "mp3", "wav", "flac", "aac", "ogg", "wma", "m4a", "opus", "aiff", "alac",
    };
    static const std::unordered_set<std::string> kImage = {
        "jpg", "jpeg", "png", "gif", "bmp", "webp", "tiff", "tif", "svg", "heic", "ico",
    };
    static const std::unordered_set<std::string> kDocument = {
        "pdf", "doc", "docx", "odt", "rtf", "epub", "xls", "xlsx", "ppt", "pptx",
    };
    static const std::unordered_set<std::string> kText = {
        "txt", "md", "markdown", "csv", "json", "xml", "yaml", "yml", "log", "ini",
    };
    static const std::unordered_set<std::string> kArchive = {
        "zip", "rar", "7z", "tar", "gz", "bz2", "xz", "tgz",
    };

    if (kVideo.count(ext)) return FileCategory::Video;
    if (kAudio.count(ext)) return FileCategory::Audio;
    if (kImage.count(ext)) return FileCategory::Image;
    if (kDocument.count(ext)) return FileCategory::Document;
    if (kText.count(ext)) return FileCategory::Text;
    if (kArchive.count(ext)) return FileCategory::Archive;
    return FileCategory::Unknown;
}

// Best-effort MIME type guess (IFileSystem::Inspect's contract explicitly allows
// nullopt) -- not a substitute for real content sniffing.
std::optional<std::string> GuessMimeType(const std::string& ext) {
    static const std::unordered_map<std::string, std::string> kMimeTypes = {
        {"mp4", "video/mp4"}, {"mkv", "video/x-matroska"}, {"avi", "video/x-msvideo"},
        {"mov", "video/quicktime"}, {"webm", "video/webm"}, {"wmv", "video/x-ms-wmv"},
        {"mp3", "audio/mpeg"}, {"wav", "audio/wav"}, {"flac", "audio/flac"},
        {"aac", "audio/aac"}, {"ogg", "audio/ogg"}, {"m4a", "audio/mp4"}, {"opus", "audio/opus"},
        {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"png", "image/png"}, {"gif", "image/gif"},
        {"bmp", "image/bmp"}, {"webp", "image/webp"}, {"svg", "image/svg+xml"},
        {"pdf", "application/pdf"}, {"zip", "application/zip"}, {"json", "application/json"},
        {"xml", "application/xml"}, {"txt", "text/plain"}, {"md", "text/markdown"},
        {"html", "text/html"}, {"htm", "text/html"}, {"csv", "text/csv"},
    };
    const auto it = kMimeTypes.find(ext);
    if (it == kMimeTypes.end()) {
        return std::nullopt;
    }
    return it->second;
}

}  // namespace

bool LocalFileSystem::Exists(const std::string& path) const {
    std::error_code ec;
    return stdfs::exists(path, ec);
}

FileInfo LocalFileSystem::Inspect(const std::string& path) const {
    std::error_code existsEc;
    if (!stdfs::exists(path, existsEc)) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_FILE_NOT_FOUND", errors::ErrorCategory::FileNotFound,
            "File not found: " + path, existsEc.message()));
    }

    FileInfo info;
    info.path = path;
    info.filename = GetFilename(path);
    info.extension = GetExtension(path);
    info.category = CategoryFromExtension(info.extension);

    std::error_code sizeEc;
    const auto size = stdfs::file_size(path, sizeEc);
    info.sizeBytes = sizeEc ? 0 : static_cast<std::uint64_t>(size);

    info.mimeType = GuessMimeType(info.extension);
    return info;
}

void LocalFileSystem::Copy(const std::string& from, const std::string& to) {
    std::error_code ec;
    stdfs::copy(from, to, ec);
    if (ec) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_COPY_FAILED", errors::ErrorCategory::EngineFailure,
            "Could not copy file.", "from=" + from + " to=" + to + " error=" + ec.message()));
    }
}

void LocalFileSystem::Move(const std::string& from, const std::string& to) {
    std::error_code ec;
    stdfs::rename(from, to, ec);
    if (!ec) {
        return;
    }
    // rename() fails with EXDEV across volumes; fall back to copy + delete rather than
    // surfacing that as an error, since "move" is the operation the caller actually asked for.
    std::error_code copyEc;
    stdfs::copy(from, to, stdfs::copy_options::overwrite_existing, copyEc);
    if (copyEc) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_MOVE_FAILED", errors::ErrorCategory::EngineFailure,
            "Could not move file.", "from=" + from + " to=" + to + " error=" + copyEc.message()));
    }
    std::error_code removeEc;
    stdfs::remove(from, removeEc);  // best-effort; destination already has the data
}

void LocalFileSystem::Rename(const std::string& path, const std::string& newName) {
    const stdfs::path target = stdfs::path(path).parent_path() / newName;
    std::error_code ec;
    stdfs::rename(path, target, ec);
    if (ec) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_RENAME_FAILED", errors::ErrorCategory::EngineFailure,
            "Could not rename file.", "path=" + path + " error=" + ec.message()));
    }
}

void LocalFileSystem::Delete(const std::string& path) {
    std::error_code ec;
    stdfs::remove_all(path, ec);
    if (ec) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_DELETE_FAILED", errors::ErrorCategory::EngineFailure,
            "Could not delete path.", "path=" + path + " error=" + ec.message()));
    }
}

void LocalFileSystem::CreateDirectory(const std::string& path) {
    std::error_code ec;
    stdfs::create_directories(path, ec);
    if (ec) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_CREATE_DIR_FAILED", errors::ErrorCategory::PermissionError,
            "Could not create directory.", "path=" + path + " error=" + ec.message()));
    }
}

std::uint64_t LocalFileSystem::CalculateSize(const std::string& path) const {
    std::error_code existsEc;
    if (!stdfs::exists(path, existsEc)) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_FILE_NOT_FOUND", errors::ErrorCategory::FileNotFound,
            "Path not found: " + path, existsEc.message()));
    }

    std::error_code isDirEc;
    if (!stdfs::is_directory(path, isDirEc)) {
        std::error_code sizeEc;
        const auto size = stdfs::file_size(path, sizeEc);
        return sizeEc ? 0 : static_cast<std::uint64_t>(size);
    }

    std::uint64_t total = 0;
    std::error_code iterEc;
    for (auto it = stdfs::recursive_directory_iterator(
             path, stdfs::directory_options::skip_permission_denied, iterEc);
         it != stdfs::recursive_directory_iterator(); it.increment(iterEc)) {
        if (iterEc) {
            break;
        }
        std::error_code fileTypeEc;
        if (it->is_regular_file(fileTypeEc) && !fileTypeEc) {
            std::error_code sizeEc;
            const auto size = it->file_size(sizeEc);
            if (!sizeEc) {
                total += static_cast<std::uint64_t>(size);
            }
        }
    }
    return total;
}

std::string LocalFileSystem::GetExtension(const std::string& path) const {
    return paths::GetExtension(path);
}

std::string LocalFileSystem::GetFilename(const std::string& path) const {
    return paths::GetFilename(path);
}

std::string LocalFileSystem::GetParentDirectory(const std::string& path) const {
    return paths::GetParentDirectory(path);
}

std::optional<std::uint64_t> LocalFileSystem::GetAvailableDiskSpace(const std::string& path) const {
    stdfs::path probe(path);
    std::error_code ec;
    // std::filesystem::space() needs an existing path to stat; walk up to the nearest
    // existing ancestor (common case: checking space for an output file that hasn't
    // been created yet).
    while (!probe.empty() && !stdfs::exists(probe, ec)) {
        const stdfs::path parent = probe.parent_path();
        if (parent == probe) {
            break;
        }
        probe = parent;
        ec.clear();
    }
    if (probe.empty()) {
        return std::nullopt;
    }

    const auto info = stdfs::space(probe, ec);
    if (ec) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(info.available);
}

std::vector<std::string> LocalFileSystem::ListDirectory(const std::string& directory) const {
    std::vector<std::string> names;
    std::error_code existsEc;
    if (!stdfs::exists(directory, existsEc) || existsEc) {
        return names;
    }

    std::error_code iterEc;
    for (auto it = stdfs::directory_iterator(
             directory, stdfs::directory_options::skip_permission_denied, iterEc);
         it != stdfs::directory_iterator(); it.increment(iterEc)) {
        if (iterEc) {
            break;
        }
        names.push_back(it->path().filename().string());
    }
    return names;
}

}  // namespace mediatool::filesystem
