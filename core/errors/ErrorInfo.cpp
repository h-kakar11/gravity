#include "core/errors/ErrorInfo.h"

#include <stdexcept>

namespace mediatool::errors {

std::string ToWireString(ErrorCategory category) {
    switch (category) {
        case ErrorCategory::FileNotFound:
            return "FILE_NOT_FOUND";
        case ErrorCategory::InvalidFile:
            return "INVALID_FILE";
        case ErrorCategory::UnsupportedFormat:
            return "UNSUPPORTED_FORMAT";
        case ErrorCategory::EngineFailure:
            return "ENGINE_FAILURE";
        case ErrorCategory::DownloadFailure:
            return "DOWNLOAD_FAILURE";
        case ErrorCategory::NetworkError:
            return "NETWORK_ERROR";
        case ErrorCategory::PermissionError:
            return "PERMISSION_ERROR";
        case ErrorCategory::DiskSpaceError:
            return "DISK_SPACE_ERROR";
        case ErrorCategory::Cancelled:
            return "CANCELLED";
        case ErrorCategory::Unknown:
            return "UNKNOWN";
    }
    // Unreachable for a valid enum value, but MinGW warns on missing return otherwise.
    return "UNKNOWN";
}

ErrorCategory ErrorCategoryFromWireString(const std::string& wire) {
    if (wire == "FILE_NOT_FOUND") return ErrorCategory::FileNotFound;
    if (wire == "INVALID_FILE") return ErrorCategory::InvalidFile;
    if (wire == "UNSUPPORTED_FORMAT") return ErrorCategory::UnsupportedFormat;
    if (wire == "ENGINE_FAILURE") return ErrorCategory::EngineFailure;
    if (wire == "DOWNLOAD_FAILURE") return ErrorCategory::DownloadFailure;
    if (wire == "NETWORK_ERROR") return ErrorCategory::NetworkError;
    if (wire == "PERMISSION_ERROR") return ErrorCategory::PermissionError;
    if (wire == "DISK_SPACE_ERROR") return ErrorCategory::DiskSpaceError;
    if (wire == "CANCELLED") return ErrorCategory::Cancelled;
    if (wire == "UNKNOWN") return ErrorCategory::Unknown;
    throw std::invalid_argument("Unknown ErrorCategory wire string: " + wire);
}

nlohmann::json ErrorInfo::ToJson() const {
    return nlohmann::json{
        {"code", code},
        {"category", ToWireString(category)},
        {"message", message},
        {"details", details},
        {"recoverable", recoverable},
    };
}

ErrorInfo ErrorInfo::FromJson(const nlohmann::json& json) {
    ErrorInfo info;
    info.code = json.at("code").get<std::string>();
    info.category = ErrorCategoryFromWireString(json.at("category").get<std::string>());
    info.message = json.at("message").get<std::string>();
    info.details = json.at("details").get<std::string>();
    info.recoverable = json.at("recoverable").get<bool>();
    return info;
}

ErrorInfo ErrorInfo::Make(std::string code, ErrorCategory category, std::string message,
                           std::string details, bool recoverable) {
    ErrorInfo info;
    info.code = std::move(code);
    info.category = category;
    info.message = std::move(message);
    info.details = std::move(details);
    info.recoverable = recoverable;
    return info;
}

}  // namespace mediatool::errors
