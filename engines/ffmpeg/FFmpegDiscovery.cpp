#include "engines/ffmpeg/FFmpegDiscovery.h"

#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace mediatool::media {

namespace {

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
    auto resolved = ResolveViaPathLookup(runner, commandName);
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
