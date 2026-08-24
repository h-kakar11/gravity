// Retry classification and backoff (spec sections 14, 15). Both are pure functions, so
// every case here is a direct call -- classification never touches a network and backoff
// never sleeps.

#include "core/queue/BackoffPolicy.h"
#include "core/queue/RetryClassifier.h"

#include <gtest/gtest.h>

#include "core/errors/MediaToolException.h"
#include "core/queue/QueueTypes.h"

using mediatool::errors::ErrorCategory;
using mediatool::errors::ErrorInfo;
using mediatool::errors::MediaToolException;
using mediatool::queue::BackoffDelayMs;
using mediatool::queue::ClassifyRetry;
using mediatool::queue::RetryPolicy;

namespace {

ErrorInfo Error(const std::string& code, ErrorCategory category) {
    return ErrorInfo::Make(code, category, "message", "details");
}

}  // namespace

// --- classification ---------------------------------------------------------------------

TEST(RetryClassifier, NetworkErrorsAreTransient) {
    EXPECT_TRUE(ClassifyRetry(Error("E_TIMEOUT", ErrorCategory::NetworkError)).IsTransient());
    EXPECT_TRUE(ClassifyRetry(Error("E_CONN_RESET", ErrorCategory::NetworkError)).IsTransient());
}

TEST(RetryClassifier, DeterministicFailuresArePermanent) {
    // Each of these is a property of the request or the filesystem and reproduces exactly.
    const std::pair<const char*, ErrorCategory> cases[] = {
        {"E_INPUT_NOT_FOUND", ErrorCategory::FileNotFound},
        {"E_MALFORMED", ErrorCategory::InvalidFile},
        {"E_UNSUPPORTED", ErrorCategory::UnsupportedFormat},
        {"E_DENIED", ErrorCategory::PermissionError},
    };
    for (const auto& [code, category] : cases) {
        const auto classification = ClassifyRetry(Error(code, category));
        EXPECT_FALSE(classification.IsTransient()) << code;
        EXPECT_FALSE(classification.reason.empty()) << code;
    }
}

TEST(RetryClassifier, CancellationIsNeverRetried) {
    // The user asked for this. Retrying would be actively wrong.
    EXPECT_FALSE(ClassifyRetry(Error("E_CANCELLED", ErrorCategory::Cancelled)).IsTransient());
}

TEST(RetryClassifier, DiskSpaceNeedsUserActionNotATimer) {
    EXPECT_FALSE(ClassifyRetry(Error("E_NO_SPACE", ErrorCategory::DiskSpaceError)).IsTransient());
}

TEST(RetryClassifier, UnknownAndUnclassifiedDefaultToNoRetry) {
    // The governing rule: when in doubt, do not retry (spec section 14).
    EXPECT_FALSE(ClassifyRetry(Error("E_MYSTERY", ErrorCategory::Unknown)).IsTransient());
    EXPECT_FALSE(ClassifyRetry(ErrorInfo{}).IsTransient());
}

TEST(RetryClassifier, SpecificCodesOverrideTheirCategoryInBothDirections) {
    // A yt-dlp transport blip arrives as a DownloadFailure, whose default is permanent...
    EXPECT_TRUE(
        ClassifyRetry(Error("E_DOWNLOAD_TRANSPORT_ERROR", ErrorCategory::DownloadFailure))
            .IsTransient());
    EXPECT_TRUE(ClassifyRetry(Error("E_DOWNLOAD_HTTP_5XX", ErrorCategory::DownloadFailure)).IsTransient());
    EXPECT_TRUE(ClassifyRetry(Error("E_DOWNLOAD_RATE_LIMITED", ErrorCategory::DownloadFailure)).IsTransient());

    // ...and a 404 arrives as a NetworkError, whose default is transient. It will still be
    // a 404 in thirty seconds.
    EXPECT_FALSE(ClassifyRetry(Error("E_DOWNLOAD_NOT_FOUND", ErrorCategory::NetworkError)).IsTransient());
    EXPECT_FALSE(ClassifyRetry(Error("E_DOWNLOAD_UNAVAILABLE", ErrorCategory::NetworkError)).IsTransient());
    EXPECT_FALSE(ClassifyRetry(Error("E_INVALID_DOWNLOAD_URL", ErrorCategory::NetworkError)).IsTransient());
}

TEST(RetryClassifier, AmbiguousDownloadFailureDefaultsToPermanent) {
    // DownloadFailure covers both "the connection dropped" and "this video is private".
    // Without a specific code saying which, the default rule applies.
    EXPECT_FALSE(ClassifyRetry(Error("E_DOWNLOAD_WHATEVER", ErrorCategory::DownloadFailure)).IsTransient());
}

TEST(RetryClassifier, AFailedHandoffToAChildProcessIsTransient) {
    // The child never got its instructions, so nothing about the request has been shown to
    // be wrong. Reached for real: writing to a child's stdin can race the drain thread
    // reaping it, and without this the job failed permanently on a race it could have just
    // retried past.
    for (const char* code : {"E_PROCESS_WRITE_FAILED", "E_PROCESS_START_FAILED",
                             "E_FFMPEG_LAUNCH_FAILED", "E_FFPROBE_LAUNCH_FAILED"}) {
        EXPECT_TRUE(ClassifyRetry(Error(code, ErrorCategory::EngineFailure)).IsTransient()) << code;
    }
}

TEST(RetryClassifier, AStalledEngineIsWorthOneMoreTryButAWrongArgumentIsNot) {
    EXPECT_TRUE(ClassifyRetry(Error("E_FFMPEG_STALLED", ErrorCategory::EngineFailure)).IsTransient());
    EXPECT_FALSE(ClassifyRetry(Error("E_FFMPEG_FAILED", ErrorCategory::EngineFailure)).IsTransient());
}

TEST(RetryClassifier, AnInterruptedJobIsNotRetriedAutomatically) {
    // Genuinely unknown rather than permanent, but uncertainty means no automatic retry:
    // auto-restarting would re-download gigabytes without the user asking. The UI still
    // offers Retry because the error is marked recoverable.
    EXPECT_FALSE(ClassifyRetry(Error("E_JOB_INTERRUPTED", ErrorCategory::Unknown)).IsTransient());
}

TEST(RetryClassifier, EveryDecisionCarriesAReason) {
    // The reason is surfaced in the job detail panel, so a user can see why something is
    // or is not being retried rather than guessing.
    for (const auto category : {ErrorCategory::NetworkError, ErrorCategory::FileNotFound,
                                ErrorCategory::Cancelled, ErrorCategory::Unknown}) {
        EXPECT_FALSE(ClassifyRetry(Error("E_X", category)).reason.empty());
    }
}

// --- backoff -----------------------------------------------------------------------------

TEST(BackoffPolicy, GrowsExponentiallyFromTheInitialDelay) {
    RetryPolicy policy;
    policy.initialDelayMs = 1'000;
    policy.maxDelayMs = 600'000;
    policy.multiplier = 2.0;

    EXPECT_EQ(BackoffDelayMs(policy, 1), 1'000);
    EXPECT_EQ(BackoffDelayMs(policy, 2), 2'000);
    EXPECT_EQ(BackoffDelayMs(policy, 3), 4'000);
    EXPECT_EQ(BackoffDelayMs(policy, 4), 8'000);
}

TEST(BackoffPolicy, IsClampedToTheMaximum) {
    RetryPolicy policy;
    policy.initialDelayMs = 1'000;
    policy.maxDelayMs = 5'000;
    policy.multiplier = 10.0;

    EXPECT_EQ(BackoffDelayMs(policy, 1), 1'000);
    EXPECT_EQ(BackoffDelayMs(policy, 2), 5'000);
    EXPECT_EQ(BackoffDelayMs(policy, 3), 5'000);
}

TEST(BackoffPolicy, ExtremeAttemptNumbersStayClampedRatherThanOverflowing) {
    // The reason the computation runs in double and clamps: an int64 accumulator overflows
    // long before the clamp would catch it, and an overflowed delay is a scheduling bug
    // that only shows up on the unlucky run.
    RetryPolicy policy;
    policy.initialDelayMs = 1'000;
    policy.maxDelayMs = 60'000;
    policy.multiplier = 10.0;

    for (int attempt : {50, 500, 10'000, 1'000'000}) {
        const auto delay = BackoffDelayMs(policy, attempt);
        EXPECT_EQ(delay, 60'000) << "attempt " << attempt;
        EXPECT_GE(delay, 0);
    }
}

TEST(BackoffPolicy, NonPositiveAttemptsHaveNoDelay) {
    RetryPolicy policy;
    EXPECT_EQ(BackoffDelayMs(policy, 0), 0);
    EXPECT_EQ(BackoffDelayMs(policy, -5), 0);
}

TEST(BackoffPolicy, IsDeterministic) {
    // No jitter, on purpose: it would buy nothing for a single local app and would cost the
    // exact assertability the retry tests depend on.
    RetryPolicy policy;
    for (int attempt = 1; attempt <= 5; ++attempt) {
        EXPECT_EQ(BackoffDelayMs(policy, attempt), BackoffDelayMs(policy, attempt));
    }
}

TEST(BackoffPolicy, AMultiplierOfOneMeansAFixedDelay) {
    RetryPolicy policy;
    policy.initialDelayMs = 3'000;
    policy.maxDelayMs = 60'000;
    policy.multiplier = 1.0;

    EXPECT_EQ(BackoffDelayMs(policy, 1), 3'000);
    EXPECT_EQ(BackoffDelayMs(policy, 7), 3'000);
}

// --- policy validation ---------------------------------------------------------------------

TEST(RetryPolicyValidation, AcceptsTheDefaults) {
    EXPECT_NO_THROW(RetryPolicy{}.Validate());
}

TEST(RetryPolicyValidation, RejectsAnUnboundedOrNegativeRetryCount) {
    // Spec section 13 forbids unbounded retries, so the ceiling is enforced here rather
    // than left to callers to remember.
    RetryPolicy policy;
    policy.maxRetries = -1;
    EXPECT_THROW(policy.Validate(), MediaToolException);
    policy.maxRetries = 1'000'000;
    EXPECT_THROW(policy.Validate(), MediaToolException);
}

TEST(RetryPolicyValidation, RejectsAMaximumBelowTheInitialDelay) {
    RetryPolicy policy;
    policy.initialDelayMs = 10'000;
    policy.maxDelayMs = 1'000;
    EXPECT_THROW(policy.Validate(), MediaToolException);
}

TEST(RetryPolicyValidation, RejectsAShrinkingOrAbsurdMultiplier) {
    RetryPolicy policy;
    policy.multiplier = 0.5;  // would make each retry sooner than the last
    EXPECT_THROW(policy.Validate(), MediaToolException);
    policy.multiplier = 1'000.0;
    EXPECT_THROW(policy.Validate(), MediaToolException);
}

TEST(RetryPolicyValidation, RoundTripsThroughJson) {
    RetryPolicy policy;
    policy.maxRetries = 4;
    policy.initialDelayMs = 1'500;
    policy.maxDelayMs = 90'000;
    policy.multiplier = 3.0;

    const RetryPolicy restored = RetryPolicy::FromJson(policy.ToJson());
    EXPECT_EQ(restored.maxRetries, 4);
    EXPECT_EQ(restored.initialDelayMs, 1'500);
    EXPECT_EQ(restored.maxDelayMs, 90'000);
    EXPECT_DOUBLE_EQ(restored.multiplier, 3.0);
}

TEST(RetryPolicyValidation, RejectsOutOfRangeValuesArrivingAsJson) {
    // This is the path IPC input takes, so it validates rather than clamping (section 54).
    EXPECT_THROW(RetryPolicy::FromJson({{"maxRetries", 9999}}), MediaToolException);
    EXPECT_THROW(RetryPolicy::FromJson({{"multiplier", 0.1}}), MediaToolException);
}

TEST(RetryPolicyValidation, IgnoresUnknownAndWronglyTypedFields) {
    // A state file from a newer build must load rather than throw.
    const auto policy = RetryPolicy::FromJson(
        {{"maxRetries", 2}, {"somethingNew", "value"}, {"initialDelayMs", "not a number"}});
    EXPECT_EQ(policy.maxRetries, 2);
    EXPECT_EQ(policy.initialDelayMs, RetryPolicy{}.initialDelayMs);
}

// --- duplicate keys ---------------------------------------------------------------------------

TEST(DuplicateKey, IdenticalRequestsProduceIdenticalKeys) {
    using mediatool::jobs::JobType;
    using mediatool::queue::MakeDuplicateKey;

    const nlohmann::json a{{"url", "https://example.com/v"}, {"quality", "BEST"}};
    const nlohmann::json b{{"quality", "BEST"}, {"url", "https://example.com/v"}};

    // Key order in the source object must not matter -- the frontend is free to serialize
    // its params in any order.
    EXPECT_EQ(MakeDuplicateKey(JobType::Download, a), MakeDuplicateKey(JobType::Download, b));
}

TEST(DuplicateKey, DifferentParamsOrTypesProduceDifferentKeys) {
    using mediatool::jobs::JobType;
    using mediatool::queue::MakeDuplicateKey;

    const nlohmann::json params{{"url", "https://example.com/v"}, {"quality", "BEST"}};
    const nlohmann::json other{{"url", "https://example.com/v"}, {"quality", "AUDIO_ONLY"}};

    EXPECT_NE(MakeDuplicateKey(JobType::Download, params),
              MakeDuplicateKey(JobType::Download, other));
    // Same params, different operation -- converting and compressing the same file are not
    // the same request.
    EXPECT_NE(MakeDuplicateKey(JobType::Conversion, params),
              MakeDuplicateKey(JobType::Compression, params));
}
