#include "engines/ffmpeg/FFmpegDiscovery.h"

#include <vector>

namespace mediatool::media {

namespace {

std::string TrimTrailingWhitespace(const std::string& value) {
    std::string result = value;
    while (!result.empty() &&
           (result.back() == '\r' || result.back() == '\n' || result.back() == ' ' ||
            result.back() == '\t')) {
        result.pop_back();
    }
    return result;
}

// Runs `where <commandName>` and returns the first path it prints, or nullopt if the
// command isn't found on PATH or the runner itself fails to launch `where`. Deliberately
// swallows every exception: this function's entire contract is "never throw".
std::optional<std::string> ResolveViaWhere(process::IProcessRunner& runner,
                                            const std::string& commandName) {
    std::vector<std::string> matches;
    try {
        process::ProcessOptions options;
        auto proc = runner.Start(
            "where", {commandName}, options,
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

std::optional<std::string> DiscoverExecutable(process::IProcessRunner& runner,
                                               const std::optional<std::string>& overridePath,
                                               const std::string& commandName) {
    if (overridePath.has_value() && !overridePath->empty()) {
        return overridePath;
    }
    return ResolveViaWhere(runner, commandName);
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

}  // namespace mediatool::media
