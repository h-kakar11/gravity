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
