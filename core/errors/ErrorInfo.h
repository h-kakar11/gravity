#pragma once

// Structured error model (spec section 26, docs/ipc-contract.md). Every failure that can
// reach the frontend must be represented as an ErrorInfo -- never a bare exception message
// or errno. `message` is user-facing; `details` is for developer diagnostics (stderr
// output, exception text, stack context) and is never required to be user-friendly.

#include <string>

#include <nlohmann/json.hpp>

namespace mediatool::errors {

enum class ErrorCategory {
    FileNotFound,
    InvalidFile,
    UnsupportedFormat,
    EngineFailure,
    DownloadFailure,
    NetworkError,
    PermissionError,
    DiskSpaceError,
    Cancelled,
    Unknown,
};

std::string ToWireString(ErrorCategory category);
ErrorCategory ErrorCategoryFromWireString(const std::string& wire);

struct ErrorInfo {
    std::string code;            // short machine token, e.g. "E_FFMPEG_LAUNCH_FAILED"
    ErrorCategory category = ErrorCategory::Unknown;
    std::string message;         // user-facing explanation
    std::string details;         // developer diagnostics
    bool recoverable = false;

    nlohmann::json ToJson() const;
    static ErrorInfo FromJson(const nlohmann::json& json);

    static ErrorInfo Make(std::string code, ErrorCategory category, std::string message,
                           std::string details = "", bool recoverable = false);
};

}  // namespace mediatool::errors
