#include "engines/ffmpeg/FFmpegProgressParser.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <sstream>

namespace mediatool::media {

namespace {

std::string Trim(const std::string& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

std::optional<double> ParseDouble(const std::string& value) {
    try {
        size_t consumed = 0;
        double parsed = std::stod(value, &consumed);
        if (consumed == 0) return std::nullopt;
        return parsed;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::uint64_t> ParseUInt64(const std::string& value) {
    try {
        size_t consumed = 0;
        unsigned long long parsed = std::stoull(value, &consumed);
        if (consumed == 0) return std::nullopt;
        return static_cast<std::uint64_t>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

// "1.02x" -> 1.02, "N/A" -> nullopt (ffmpeg prints N/A for speed before the first frame).
std::optional<double> ParseSpeed(const std::string& value) {
    std::string trimmed = value;
    if (!trimmed.empty() && trimmed.back() == 'x') trimmed.pop_back();
    return ParseDouble(trimmed);
}

}  // namespace

FFmpegProgressParser::FFmpegProgressParser(std::optional<double> totalDurationSeconds,
                                           std::optional<double> inputBitrateBps)
    : totalDurationSeconds_(totalDurationSeconds), inputBitrateBps_(inputBitrateBps) {}

void FFmpegProgressParser::FeedLine(const std::string& line) {
    std::string trimmed = Trim(line);
    if (trimmed.empty()) return;

    auto eq = trimmed.find('=');
    if (eq == std::string::npos) return;  // malformed line; ignore rather than throw

    std::string key = trimmed.substr(0, eq);
    std::string value = trimmed.substr(eq + 1);
    fields_[key] = value;

    if (key == "progress") {
        pending_ = BuildProgress(value == "end");
        fields_.clear();
    }
}

std::optional<jobs::Progress> FFmpegProgressParser::TakeProgressIfReady() {
    if (!pending_.has_value()) return std::nullopt;
    jobs::Progress result = std::move(*pending_);
    pending_.reset();
    return result;
}

jobs::Progress FFmpegProgressParser::BuildProgress(bool isEnd) const {
    jobs::Progress progress;

    auto field = [this](const char* key) -> std::optional<std::string> {
        auto it = fields_.find(key);
        if (it == fields_.end()) return std::nullopt;
        return it->second;
    };

    // ffmpeg's out_time_ms field is a long-standing misnomer: despite the name, its value
    // is microseconds, not milliseconds (out_time_us carries the same value on newer
    // ffmpeg builds and is preferred when present).
    std::optional<double> outTimeSeconds;
    if (auto us = field("out_time_us"); us.has_value()) {
        if (auto parsed = ParseUInt64(*us)) outTimeSeconds = static_cast<double>(*parsed) / 1'000'000.0;
    } else if (auto ms = field("out_time_ms"); ms.has_value()) {
        if (auto parsed = ParseUInt64(*ms)) outTimeSeconds = static_cast<double>(*parsed) / 1'000'000.0;
    }

    std::optional<double> speedFactor;
    if (auto speed = field("speed")) speedFactor = ParseSpeed(*speed);

    if (auto totalSize = field("total_size")) {
        progress.processedBytes = ParseUInt64(*totalSize);
    }

    if (speedFactor.has_value() && inputBitrateBps_.has_value() && *inputBitrateBps_ > 0.0 && !isEnd) {
        progress.speedBytesPerSecond = *speedFactor * (*inputBitrateBps_ / 8.0);
    }

    if (isEnd) {
        progress.percentage = 100.0;
    } else if (outTimeSeconds.has_value() && totalDurationSeconds_.has_value() &&
               *totalDurationSeconds_ > 0.0) {
        double pct = (*outTimeSeconds / *totalDurationSeconds_) * 100.0;
        progress.percentage = std::min(std::max(pct, 0.0), 100.0);
    }

    if (outTimeSeconds.has_value() && totalDurationSeconds_.has_value() &&
        speedFactor.has_value() && *speedFactor > 0.0 && !isEnd) {
        double remaining = *totalDurationSeconds_ - *outTimeSeconds;
        if (remaining > 0.0) progress.etaSeconds = remaining / *speedFactor;
    }

    if (auto frame = field("frame")) {
        progress.currentItem = "frame " + *frame;
    }

    std::ostringstream status;
    if (isEnd) {
        status << "Completed";
    } else {
        status << "Encoding";
        if (auto frame = field("frame")) status << " frame " << *frame;
        if (speedFactor.has_value()) status << " (" << *speedFactor << "x)";
    }
    progress.statusMessage = status.str();

    return progress;
}

}  // namespace mediatool::media
