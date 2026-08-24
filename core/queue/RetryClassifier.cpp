#include "core/queue/RetryClassifier.h"

#include <unordered_set>

namespace mediatool::queue {

namespace {

using errors::ErrorCategory;

// Error codes that are transient despite sitting in a category that usually is not.
// yt-dlp's transport failures arrive as DownloadFailure rather than NetworkError, and an
// engine that was momentarily unable to start is worth one more try.
const std::unordered_set<std::string>& TransientCodes() {
    static const std::unordered_set<std::string> kCodes{
        "E_DOWNLOAD_TRANSPORT_ERROR",   // connection reset / read timeout mid-transfer
        "E_DOWNLOAD_HTTP_5XX",          // the origin server is having a bad minute
        "E_DOWNLOAD_RATE_LIMITED",      // HTTP 429; backing off is exactly the right move
        "E_DOWNLOAD_INCOMPLETE",        // stream truncated before the expected byte count
        "E_FFMPEG_STALLED",             // child wedged and was killed; may not repeat
    };
    return kCodes;
}

// Error codes that are permanent even though their category is usually retryable. A 404
// arrives as a NetworkError but will still be a 404 in thirty seconds.
const std::unordered_set<std::string>& PermanentCodes() {
    static const std::unordered_set<std::string> kCodes{
        "E_DOWNLOAD_NOT_FOUND",       // removed/private video
        "E_DOWNLOAD_UNAVAILABLE",     // geo-blocked, age-gated, members-only
        "E_DOWNLOAD_HTTP_4XX",        // client error other than 429
        "E_INVALID_DOWNLOAD_URL",
        // A job that was mid-flight when the process died. Genuinely *unknown* rather than
        // permanent -- but the classifier has two buckets, and section 14's rule is that
        // uncertainty means no automatic retry. Auto-restarting these would re-download
        // gigabytes, or re-run an encode over a partial file, without the user asking. The
        // error is marked recoverable so the UI offers Retry; the decision stays theirs.
        // See ApplyRestartRecovery in QueuePersistence.h.
        "E_JOB_INTERRUPTED",
    };
    return kCodes;
}

}  // namespace

std::string ToWireString(RetryDisposition disposition) {
    return disposition == RetryDisposition::Transient ? "TRANSIENT" : "PERMANENT";
}

RetryClassification ClassifyRetry(const errors::ErrorInfo& error) {
    const auto permanent = [](std::string reason) {
        return RetryClassification{RetryDisposition::Permanent, std::move(reason)};
    };
    const auto transient = [](std::string reason) {
        return RetryClassification{RetryDisposition::Transient, std::move(reason)};
    };

    // Code-level overrides win over the category default in both directions, because the
    // code is the more specific statement about what actually happened.
    if (PermanentCodes().count(error.code) > 0)
        return permanent(error.code + " is a permanent failure");
    if (TransientCodes().count(error.code) > 0)
        return transient(error.code + " is a known transient failure");

    switch (error.category) {
        case ErrorCategory::NetworkError:
            // Timeouts, connection resets, DNS hiccups. The canonical retryable case.
            return transient("network errors are usually temporary");

        case ErrorCategory::DownloadFailure:
            // Ambiguous by nature: covers both "the connection dropped" and "this video is
            // private". Without a specific code above saying which, the default rule
            // applies -- don't retry (spec section 14).
            return permanent(
                "download failures are not retried unless a specific transient code says so");

        case ErrorCategory::Cancelled:
            // The user asked for this. Retrying would be actively wrong.
            return permanent("the job was cancelled deliberately");

        case ErrorCategory::FileNotFound:
        case ErrorCategory::InvalidFile:
        case ErrorCategory::UnsupportedFormat:
        case ErrorCategory::PermissionError:
            // Every one of these is a property of the request or the filesystem, and is
            // exactly reproducible: a missing input stays missing, an unsupported format
            // stays unsupported, a denied path stays denied.
            return permanent("this failure is deterministic and would repeat identically");

        case ErrorCategory::DiskSpaceError:
            // Space *can* free up, but retrying on a timer will almost always just fail
            // again and hide the real problem behind a stack of attempts. The user needs to
            // act, so surface it immediately and offer manual retry.
            return permanent("free space needs user action, not an automatic retry");

        case ErrorCategory::EngineFailure:
            // Covers a genuine ffmpeg bad-arguments failure (permanent) and a transient
            // launch/stall problem (handled by code above). Unknown engine failures take
            // the safe default.
            return permanent("engine failures are treated as deterministic by default");

        case ErrorCategory::Unknown:
            break;
    }

    return permanent("unclassified errors are not retried automatically");
}

}  // namespace mediatool::queue
