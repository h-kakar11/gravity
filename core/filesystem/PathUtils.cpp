#include "core/filesystem/PathUtils.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace stdfs = std::filesystem;

namespace mediatool::filesystem::paths {

std::string Join(const std::string& base, const std::string& component) {
    return (stdfs::path(base) / component).string();
}

std::string Join(const std::string& base, const std::vector<std::string>& components) {
    stdfs::path result(base);
    for (const auto& component : components) {
        result /= component;
    }
    return result.string();
}

bool IsAbsolute(const std::string& path) {
    return stdfs::path(path).is_absolute();
}

bool IsUncPath(const std::string& path) {
    // "\\server\share..." or "//server/share..." -- two leading separators (either
    // slash direction; Windows accepts both) followed by more path.
    return path.size() >= 2 && (path[0] == '\\' || path[0] == '/') &&
           (path[1] == '\\' || path[1] == '/');
}

namespace {

// Host-platform-independent traversal check: splits on either separator (Windows accepts
// both) and looks for a literal ".." segment, rather than relying on
// std::filesystem::path::lexically_normal() -- which, like IsAbsolute() above, answers
// according to the *host's* path grammar and would silently fail to collapse "C:\a\..\b"
// at all when built on a POSIX host (backslash isn't a separator there).
bool ContainsTraversalSegment(const std::string& path) {
    std::size_t start = 0;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '\\' || path[i] == '/') {
            if (path.compare(start, i - start, "..") == 0) return true;
            start = i + 1;
        }
    }
    return false;
}

}  // namespace

bool LooksAbsoluteWindowsPath(const std::string& path) {
    if (IsUncPath(path)) return true;
    // "C:\..." or "C:/..." -- a drive letter followed by a colon and a separator. Not
    // "C:" alone or "C:foo" (a drive-relative path on Windows, not absolute).
    if (path.size() >= 3 &&
        ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && (path[2] == '\\' || path[2] == '/')) {
        return true;
    }
    return false;
}

bool IsSafeUserSuppliedPath(const std::string& path, bool allowNetworkPaths) {
    if (path.empty()) return false;
    if (!allowNetworkPaths && IsUncPath(path)) return false;
    if (!LooksAbsoluteWindowsPath(path)) return false;
    if (ContainsTraversalSegment(path)) return false;
    return true;
}

std::string Normalize(const std::string& path) {
    return stdfs::path(path).lexically_normal().string();
}

std::string GetExtension(const std::string& path) {
    std::string ext = stdfs::path(path).extension().string();
    if (!ext.empty() && ext.front() == '.') {
        ext.erase(0, 1);
    }
    std::transform(ext.begin(), ext.end(), ext.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::string GetFilename(const std::string& path) {
    return stdfs::path(path).filename().string();
}

std::string GetParentDirectory(const std::string& path) {
    return stdfs::path(path).parent_path().string();
}

}  // namespace mediatool::filesystem::paths
