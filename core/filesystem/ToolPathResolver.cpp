#include "core/filesystem/ToolPathResolver.h"

#include <algorithm>
#include <filesystem>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace stdfs = std::filesystem;

namespace mediatool::filesystem {

std::string CleanEnvPathValue(const std::string& raw) {
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    std::size_t begin = 0;
    std::size_t end = raw.size();
    while (begin < end && isSpace(static_cast<unsigned char>(raw[begin]))) ++begin;
    while (end > begin && isSpace(static_cast<unsigned char>(raw[end - 1]))) --end;
    if (end - begin >= 2 && raw[begin] == '"' && raw[end - 1] == '"') {
        ++begin;
        --end;
    }
    return raw.substr(begin, end - begin);
}

std::vector<std::string> BuildToolCandidates(const std::string& envValue,
                                             const std::string& executableDirectory,
                                             const std::vector<std::string>& relativeCandidates) {
    std::vector<std::string> candidates;
    std::unordered_set<std::string> seen;
    const auto add = [&](const std::string& path) {
        if (path.empty()) return;
        const std::string native = stdfs::path(path).make_preferred().string();
        if (seen.insert(native).second) candidates.push_back(native);
    };

    add(CleanEnvPathValue(envValue));

    if (!executableDirectory.empty()) {
        stdfs::path base(executableDirectory);
        for (int level = 0; level <= kMaxAncestorLevels; ++level) {
            for (const std::string& relative : relativeCandidates) {
                add((base / relative).string());
            }
            const stdfs::path parent = base.parent_path();
            // parent_path() of a root is the root itself -- stop rather than spin.
            if (parent.empty() || parent == base) break;
            base = parent;
        }
    }

    for (const std::string& relative : relativeCandidates) add(relative);
    return candidates;
}

std::optional<std::string> FirstExisting(const std::vector<std::string>& candidates,
                                          const std::function<bool(const std::string&)>& exists) {
    for (const std::string& candidate : candidates) {
        if (exists(candidate)) return candidate;
    }
    return std::nullopt;
}

std::string ExecutableDirectory() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) return {};
        if (written < buffer.size()) {
            buffer.resize(written);
            break;
        }
        // Truncated (a path longer than the current buffer) -- grow and retry.
        if (buffer.size() >= 32768) return {};
        buffer.resize(buffer.size() * 2);
    }
    std::error_code ec;
    const stdfs::path parent = stdfs::path(buffer).parent_path();
    return ec ? std::string() : parent.string();
#else
    std::error_code ec;
    const stdfs::path self = stdfs::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    return self.parent_path().string();
#endif
}

}  // namespace mediatool::filesystem
