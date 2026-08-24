#include "core/filesystem/ExecutablePath.h"

#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <climits>
#endif

namespace mediatool::filesystem {

namespace {

namespace stdfs = std::filesystem;

#ifdef _WIN32

std::optional<std::string> ResolveExecutablePath() {
    // MAX_PATH is the legacy limit; a handful of doublings covers any realistic install
    // path (including one with a very long, space-containing directory name) without
    // hardcoding an arbitrary large buffer up front.
    std::wstring buffer(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 4; ++attempt) {
        const DWORD written =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) return std::nullopt;
        if (written < buffer.size()) {
            buffer.resize(written);
            return stdfs::path(buffer).string();
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::nullopt;
}

#else  // POSIX -- exercised by this repository's Linux dev/CI build.

std::optional<std::string> ResolveExecutablePath() {
    std::error_code ec;
    stdfs::path resolved = stdfs::read_symlink("/proc/self/exe", ec);
    if (ec || resolved.empty()) return std::nullopt;
    return resolved.string();
}

#endif

}  // namespace

std::optional<std::string> ExecutableDirectory() {
    auto exePath = ResolveExecutablePath();
    if (!exePath.has_value()) return std::nullopt;
    return stdfs::path(*exePath).parent_path().string();
}

}  // namespace mediatool::filesystem
