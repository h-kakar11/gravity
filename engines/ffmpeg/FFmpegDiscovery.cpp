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

std::set<std::string> DiscoverAvailableEncoders(process::IProcessRunner& runner, const std::string& ffmpegPath) {
    std::set<std::string> encoders;
    try {
        std::vector<std::string> lines;
        process::ProcessOptions options;
        auto proc = runner.Start(
            ffmpegPath, {"-hide_banner", "-encoders"}, options,
            [&lines](const std::string& line) { lines.push_back(line); }, [](const std::string&) {});
        if (!proc) return encoders;
        proc->Wait();

        // `ffmpeg -encoders` prints a legend, then a "------" separator, then one line per
        // encoder: six capability-flag characters (e.g. "V....."), whitespace, the encoder
        // name, whitespace, a free-text description. Only the separator-onward lines that
        // actually match that six-flag-then-name shape are treated as encoder rows.
        bool pastSeparator = false;
        for (const auto& rawLine : lines) {
            if (!pastSeparator) {
                if (rawLine.find("------") != std::string::npos) pastSeparator = true;
                continue;
            }
            const std::size_t start = rawLine.find_first_not_of(' ');
            if (start == std::string::npos) continue;
            const std::string trimmed = rawLine.substr(start);

            const std::size_t firstSpace = trimmed.find(' ');
            if (firstSpace != 6) continue;  // not a well-formed "FLAGS NAME ..." row

            const std::size_t nameStart = trimmed.find_first_not_of(' ', firstSpace);
            if (nameStart == std::string::npos) continue;
            const std::size_t nameEnd = trimmed.find(' ', nameStart);
            const std::string name =
                nameEnd == std::string::npos ? trimmed.substr(nameStart) : trimmed.substr(nameStart, nameEnd - nameStart);
            if (!name.empty()) encoders.insert(name);
        }
    } catch (...) {
        // Contract is "never throw" -- an unparseable/unlaunchable ffmpeg just yields an
        // empty set, which callers already treat as "assume only the bundled default."
    }
    return encoders;
}

}  // namespace mediatool::media
