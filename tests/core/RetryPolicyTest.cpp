#include "core/jobs/RetryPolicy.h"

#include <gtest/gtest.h>

#include "core/errors/ErrorInfo.h"

namespace mediatool::jobs {
namespace {

using errors::ErrorCategory;
using errors::ErrorInfo;

ErrorInfo Failure(ErrorCategory category, bool recoverable, const char* code = "E_TEST") {
    return ErrorInfo::Make(code, category, "message", "details", recoverable);
}

TEST(RetryPolicy, ARetryNeedsBothSignalsNotEither) {
    // The producer's `recoverable` flag alone is not enough, and the category alone is not
    // enough. Each catches a class of mistake the other misses -- see RetryPolicy.h.
    EXPECT_TRUE(IsRetryableError(Failure(ErrorCategory::NetworkError, true)));

    // Recoverable, but the category says the machine is in a state a second attempt cannot
    // change. A provider that mislabels one of these would otherwise turn one clear
    // failure into three identical ones, seconds apart.
    EXPECT_FALSE(IsRetryableError(Failure(ErrorCategory::DiskSpaceError, true)));
    EXPECT_FALSE(IsRetryableError(Failure(ErrorCategory::FileNotFound, true)));
    EXPECT_FALSE(IsRetryableError(Failure(ErrorCategory::PermissionError, true)));
    EXPECT_FALSE(IsRetryableError(Failure(ErrorCategory::UnsupportedFormat, true)));
    EXPECT_FALSE(IsRetryableError(Failure(ErrorCategory::InvalidFile, true)));

    // A retryable category, but the producer knows this particular failure is permanent --
    // a DNS name that does not resolve fails identically forever.
    EXPECT_FALSE(IsRetryableError(Failure(ErrorCategory::NetworkError, false)));
    EXPECT_FALSE(IsRetryableError(Failure(ErrorCategory::DownloadFailure, false)));
}

TEST(RetryPolicy, ACancelledJobIsNeverRetried) {
    // The most important veto: a user pressed Cancel. Retrying that is the app arguing
    // with them.
    EXPECT_FALSE(IsRetryableError(Failure(ErrorCategory::Cancelled, true)));
}

TEST(RetryPolicy, TheMergeFailuresSplitTheWayTheyWereClassified) {
    // The two halves of the Phase A downloader classification, checked end to end against
    // the policy that consumes them -- the reason that split was worth making.
    EXPECT_FALSE(IsRetryableError(
        Failure(ErrorCategory::EngineFailure, false, "E_MERGE_TOOL_MISSING")));
    EXPECT_TRUE(IsRetryableError(
        Failure(ErrorCategory::NetworkError, true, "E_FRAGMENT_DOWNLOAD_FAILED")));
    // And the deferred-operation error, which must never be retried.
    EXPECT_FALSE(IsRetryableError(
        Failure(ErrorCategory::UnsupportedFormat, false, "E_NOT_IMPLEMENTED")));
}

TEST(RetryPolicy, BackoffGrowsExponentiallyAndStopsAtTheLimit) {
    RetryPolicy policy;
    policy.maxAttempts = 4;
    policy.initialBackoff = std::chrono::milliseconds(1000);
    policy.backoffMultiplier = 2.0;
    policy.maxBackoff = std::chrono::milliseconds(60000);
    const ErrorInfo error = Failure(ErrorCategory::NetworkError, true);

    const RetryDecision first = DecideRetry(error, 1, policy);
    ASSERT_TRUE(first.shouldRetry);
    EXPECT_EQ(first.delay, std::chrono::milliseconds(1000));

    EXPECT_EQ(DecideRetry(error, 2, policy).delay, std::chrono::milliseconds(2000));
    EXPECT_EQ(DecideRetry(error, 3, policy).delay, std::chrono::milliseconds(4000));

    // The fourth attempt has run; the limit is four total, so there is no fifth.
    const RetryDecision exhausted = DecideRetry(error, 4, policy);
    EXPECT_FALSE(exhausted.shouldRetry);
    EXPECT_EQ(exhausted.delay, std::chrono::milliseconds(0));
    EXPECT_NE(exhausted.reason.find("limit"), std::string::npos);
}

TEST(RetryPolicy, BackoffIsCappedSoALongPolicyCannotScheduleAnHourOut) {
    RetryPolicy policy;
    policy.maxAttempts = 10;
    policy.initialBackoff = std::chrono::milliseconds(1000);
    policy.backoffMultiplier = 10.0;
    policy.maxBackoff = std::chrono::milliseconds(30000);
    const ErrorInfo error = Failure(ErrorCategory::NetworkError, true);

    EXPECT_EQ(DecideRetry(error, 1, policy).delay, std::chrono::milliseconds(1000));
    EXPECT_EQ(DecideRetry(error, 2, policy).delay, std::chrono::milliseconds(10000));
    // 100s uncapped -- clamped, not wrapped or negative.
    EXPECT_EQ(DecideRetry(error, 3, policy).delay, std::chrono::milliseconds(30000));
    EXPECT_EQ(DecideRetry(error, 8, policy).delay, std::chrono::milliseconds(30000));
}

TEST(RetryPolicy, MaxAttemptsOfOneDisablesRetryEntirely) {
    // "1" reads as the total number of attempts, which is why the floor is 1 and not 0:
    // zero attempts would describe a job that never runs.
    RetryPolicy policy;
    policy.maxAttempts = 1;
    EXPECT_FALSE(DecideRetry(Failure(ErrorCategory::NetworkError, true), 1, policy).shouldRetry);
}

TEST(RetryPolicy, EveryDecisionExplainsItselfEvenWhenItSaysNo) {
    // "Why did this not retry" is the question that actually gets asked, and answering it
    // from the decision beats reconstructing it from the error afterwards.
    RetryPolicy policy;
    const RetryDecision permanent =
        DecideRetry(Failure(ErrorCategory::DiskSpaceError, true, "E_DISK_FULL"), 1, policy);
    EXPECT_FALSE(permanent.shouldRetry);
    EXPECT_NE(permanent.reason.find("E_DISK_FULL"), std::string::npos);

    const RetryDecision retrying =
        DecideRetry(Failure(ErrorCategory::NetworkError, true, "E_NETWORK"), 1, policy);
    EXPECT_TRUE(retrying.shouldRetry);
    EXPECT_NE(retrying.reason.find("E_NETWORK"), std::string::npos);
}

}  // namespace
}  // namespace mediatool::jobs
