#include "engines/ffmpeg/FFmpegErrorClassifier.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>

namespace mediatool::media {

namespace {

using errors::ErrorCategory;
using errors::ErrorInfo;

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

struct Classification {
    const char* substring;  // already lowercase
    const char* code;
    ErrorCategory category;
    const char* message;  // user-facing, and says what to do about it
    bool recoverable;
};

// Checked in order, first match wins. Strings verified against ffmpeg 6.1.1's actual
// output where they could be produced locally (a truncated mp4, an unknown encoder, a
// missing input); the errno-derived ones ("No space left on device", "Permission denied")
// come from ffmpeg printing strerror() through av_err2str, the same path that produced the
// confirmed "No such file or directory".
constexpr std::array<Classification, 10> kClassifications = {{
    // Disk and permissions first: they can appear alongside a more generic message ("Error
    // opening output file"), and they are the two the user can actually fix.
    {"no space left on device", "E_DISK_FULL", ErrorCategory::DiskSpaceError,
     "There is not enough free space to write the output file.", false},
    {"permission denied", "E_PERMISSION_DENIED", ErrorCategory::PermissionError,
     "Gravity does not have permission to write to that location. Try a folder inside your "
     "user profile, or check whether antivirus software is blocking it.",
     false},
    {"operation not permitted", "E_PERMISSION_DENIED", ErrorCategory::PermissionError,
     "Gravity does not have permission to write to that location. Try a folder inside your "
     "user profile, or check whether antivirus software is blocking it.",
     false},
    {"read-only file system", "E_PERMISSION_DENIED", ErrorCategory::PermissionError,
     "That location is read-only. Choose a different output folder.", false},

    // The file itself.
    {"moov atom not found", "E_INVALID_FILE", ErrorCategory::InvalidFile,
     "This file appears to be incomplete or corrupt -- its index is missing, which usually "
     "means the download or copy did not finish.",
     false},
    {"invalid data found when processing input", "E_INVALID_FILE", ErrorCategory::InvalidFile,
     "This file is not a media file Gravity can read, or it is corrupt.", false},
    {"no such file or directory", "E_FILE_NOT_FOUND", ErrorCategory::FileNotFound,
     "The file could not be found. It may have been moved or deleted since it was selected.",
     false},

    // What this build of ffmpeg can and cannot do.
    {"unknown encoder", "E_UNSUPPORTED_CODEC", ErrorCategory::UnsupportedFormat,
     "The codec this conversion needs is not available in the ffmpeg build Gravity is using.",
     false},
    {"unknown decoder", "E_UNSUPPORTED_CODEC", ErrorCategory::UnsupportedFormat,
     "This file uses a codec the ffmpeg build Gravity is using cannot read.", false},
    {"unable to find a suitable output format", "E_UNSUPPORTED_FORMAT",
     ErrorCategory::UnsupportedFormat, "That output format is not one ffmpeg can produce.",
     false},
}};

const Classification* Match(const std::string& lowercaseStderr) {
    for (const Classification& classification : kClassifications) {
        if (lowercaseStderr.find(classification.substring) != std::string::npos) {
            return &classification;
        }
    }
    return nullptr;
}

std::string WithExitCode(const std::string& stderrTail, int exitCode) {
    std::string details = "exitCode=" + std::to_string(exitCode);
    if (!stderrTail.empty()) {
        details += "\nstderr:\n" + stderrTail;
    }
    return details;
}

}  // namespace

std::string DescribeByteCount(std::uint64_t bytes) {
    static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(kUnits)) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[64];
    // No decimals for bytes: "512.0 B" reads as a rounding of something larger.
    std::snprintf(buffer, sizeof(buffer), unit == 0 ? "%.0f %s" : "%.1f %s", value, kUnits[unit]);
    return buffer;
}

errors::ErrorInfo ClassifyFfmpegFailure(const std::string& stderrTail, int exitCode,
                                         std::optional<std::uint64_t> availableBytes) {
    const std::string lowered = ToLowerAscii(stderrTail);
    const Classification* match = Match(lowered);

    if (match == nullptr) {
        return ErrorInfo::Make("E_FFMPEG_FAILED", ErrorCategory::EngineFailure,
                                "ffmpeg exited with an error while processing this file.",
                                WithExitCode(stderrTail, exitCode), /*recoverable=*/false);
    }

    std::string message = match->message;
    // Only for disk-full, and only as a concrete addition to a message that is already
    // complete without it: a number the user can compare against the file they are
    // converting beats "not enough space".
    if (std::string(match->code) == "E_DISK_FULL" && availableBytes.has_value()) {
        message += " About " + DescribeByteCount(*availableBytes) + " is free there.";
    }

    return ErrorInfo::Make(match->code, match->category, std::move(message),
                            WithExitCode(stderrTail, exitCode), match->recoverable);
}

errors::ErrorInfo ClassifyFfprobeFailure(const std::string& stderrTail, int exitCode) {
    const std::string lowered = ToLowerAscii(stderrTail);
    if (const Classification* match = Match(lowered)) {
        return ErrorInfo::Make(match->code, match->category, match->message,
                                WithExitCode(stderrTail, exitCode), match->recoverable);
    }
    // ffprobe failing to read a file IS the definition of "not something we can work
    // with", so an unrecognized probe failure is an invalid file rather than an engine
    // failure -- which also keeps it out of the retry policy's retryable categories.
    return ErrorInfo::Make("E_INVALID_FILE", ErrorCategory::InvalidFile,
                            "This file is not a media file Gravity can read, or it is corrupt.",
                            WithExitCode(stderrTail, exitCode), /*recoverable=*/false);
}

}  // namespace mediatool::media
