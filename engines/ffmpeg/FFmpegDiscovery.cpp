#include "engines/ffmpeg/FFmpegDiscovery.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "core/filesystem/ExecutablePath.h"

namespace mediatool::media {

namespace {

namespace stdfs = std::filesystem;

#ifdef _WIN32
constexpr const char* kExecutableExtension = ".exe";
#else
constexpr const char* kExecutableExtension = "";
#endif

// A packaged Gravity install ships its own FFmpeg/ffprobe next to mediatool-core.exe (see
// docs/phase-7.md "Resource discovery") rather than requiring the user to have installed
// FFmpeg separately and put it on PATH. Checked in `bin/<name>` first (the documented
// bundling location) and then directly beside the executable, since either is a
// reasonable place for a release engineer to have dropped it.
std::optional<std::string> ResolveBundled(const std::string& commandName) {
    auto exeDir = filesystem::ExecutableDirectory();
    if (!exeDir.has_value()) return std::nullopt;

    const std::string filename = commandName + kExecutableExtension;
    for (const stdfs::path& candidate :
         {stdfs::path(*exeDir) / "bin" / filename, stdfs::path(*exeDir) / filename}) {
        std::error_code ec;
        if (stdfs::is_regular_file(candidate, ec) && !ec) {
            return candidate.string();
        }
    }
    return std::nullopt;
}

// Windows has no `which`; POSIX has no `where`. One lookup helper, two spellings of the
// same idea -- this stays a single discovery path (see docs/decisions.md "FFmpeg discovery
// is single-source"), it just knows which tool the host actually ships.
#ifdef _WIN32
constexpr const char* kLookupCommand = "where";
#else
constexpr const char* kLookupCommand = "which";
#endif

std::string TrimTrailingWhitespace(const std::string& value) {
    std::string result = value;
    while (!result.empty() &&
           (result.back() == '\r' || result.back() == '\n' || result.back() == ' ' ||
            result.back() == '\t')) {
        result.pop_back();
    }
    return result;
}

// Runs the host's PATH-lookup command and returns the first path it prints, or nullopt if
// the command isn't found on PATH or the runner itself fails to launch it. Deliberately
// swallows every exception: this function's entire contract is "never throw".
std::optional<std::string> ResolveViaPathLookup(process::IProcessRunner& runner,
                                                 const std::string& commandName) {
    std::vector<std::string> matches;
    try {
        process::ProcessOptions options;
        auto proc = runner.Start(
            kLookupCommand, {commandName}, options,
            [&matches](const std::string& line) {
                auto trimmed = TrimTrailingWhitespace(line);
                if (!trimmed.empty()) {
                    matches.push_back(trimmed);
                }
            },
            [](const std::string&) { /* ignore stderr */ });
        if (!proc) {
            return std::nullopt;
        }
        auto result = proc->Wait();
        if (result.exitCode != 0 || matches.empty()) {
            return std::nullopt;
        }
        return matches.front();
    } catch (...) {
        return std::nullopt;
    }
}

// Resolution is a child-process launch, and the queue calls IsAvailable()/Probe() often
// enough (once per job, several times per encode) that paying it every time is a real cost
// for a value that effectively never changes within a session. Cache the successful
// answer, keyed by command name. A *failed* lookup is deliberately NOT cached: a user who
// installs ffmpeg while the app is open should not have to restart it.
std::mutex g_discoveryCacheMutex;
std::map<std::string, std::string> g_discoveryCache;

std::optional<std::string> DiscoverExecutable(process::IProcessRunner& runner,
                                               const std::optional<std::string>& overridePath,
                                               const std::string& commandName) {
    if (overridePath.has_value() && !overridePath->empty()) {
        return overridePath;
    }
    {
        std::lock_guard<std::mutex> lock(g_discoveryCacheMutex);
        const auto it = g_discoveryCache.find(commandName);
        if (it != g_discoveryCache.end()) return it->second;
    }
    // Bundled first (spec: "do not assume FFmpeg is installed separately"), a system PATH
    // install second -- so a user who already has FFmpeg on PATH still works exactly as
    // before, but the packaged app does not require them to.
    auto resolved = ResolveBundled(commandName);
    if (!resolved.has_value()) {
        resolved = ResolveViaPathLookup(runner, commandName);
    }
    if (resolved.has_value()) {
        std::lock_guard<std::mutex> lock(g_discoveryCacheMutex);
        g_discoveryCache[commandName] = *resolved;
    }
    return resolved;
}

}  // namespace

std::optional<std::string> DiscoverFfmpegPath(process::IProcessRunner& runner,
                                               const std::optional<std::string>& overridePath) {
    return DiscoverExecutable(runner, overridePath, "ffmpeg");
}

std::optional<std::string> DiscoverFfprobePath(process::IProcessRunner& runner,
                                                const std::optional<std::string>& overridePath) {
    return DiscoverExecutable(runner, overridePath, "ffprobe");
}

void ResetDiscoveryCacheForTesting() {
    std::lock_guard<std::mutex> lock(g_discoveryCacheMutex);
    g_discoveryCache.clear();
}

}  // namespace mediatool::media
