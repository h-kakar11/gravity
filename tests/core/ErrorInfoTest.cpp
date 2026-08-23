#include "core/errors/ErrorInfo.h"

#include <stdexcept>

#include <gtest/gtest.h>

namespace mediatool::errors {
namespace {

struct CategoryCase {
    ErrorCategory category;
    const char* wire;
};

constexpr CategoryCase kAllCategories[] = {
    {ErrorCategory::FileNotFound, "FILE_NOT_FOUND"},
    {ErrorCategory::InvalidFile, "INVALID_FILE"},
    {ErrorCategory::UnsupportedFormat, "UNSUPPORTED_FORMAT"},
    {ErrorCategory::EngineFailure, "ENGINE_FAILURE"},
    {ErrorCategory::DownloadFailure, "DOWNLOAD_FAILURE"},
    {ErrorCategory::NetworkError, "NETWORK_ERROR"},
    {ErrorCategory::PermissionError, "PERMISSION_ERROR"},
    {ErrorCategory::DiskSpaceError, "DISK_SPACE_ERROR"},
    {ErrorCategory::Cancelled, "CANCELLED"},
    {ErrorCategory::Unknown, "UNKNOWN"},
};

TEST(ErrorInfoTest, ToWireStringMatchesContractForEveryCategory) {
    for (const auto& c : kAllCategories) {
        EXPECT_EQ(ToWireString(c.category), c.wire);
    }
}

TEST(ErrorInfoTest, FromWireStringRoundTripsForEveryCategory) {
    for (const auto& c : kAllCategories) {
        EXPECT_EQ(ErrorCategoryFromWireString(c.wire), c.category);
    }
}

TEST(ErrorInfoTest, FromWireStringThrowsOnUnknownToken) {
    EXPECT_THROW(ErrorCategoryFromWireString("NOT_A_REAL_CATEGORY"), std::invalid_argument);
}

TEST(ErrorInfoTest, ToJsonProducesExactContractShape) {
    const ErrorInfo info =
        ErrorInfo::Make("E_FFMPEG_LAUNCH_FAILED", ErrorCategory::EngineFailure,
                         "Could not start the conversion engine.", "exit code 127", true);

    const nlohmann::json json = info.ToJson();

    EXPECT_EQ(json.at("code"), "E_FFMPEG_LAUNCH_FAILED");
    EXPECT_EQ(json.at("category"), "ENGINE_FAILURE");
    EXPECT_EQ(json.at("message"), "Could not start the conversion engine.");
    EXPECT_EQ(json.at("details"), "exit code 127");
    EXPECT_EQ(json.at("recoverable"), true);
    EXPECT_EQ(json.size(), 5u);
}

TEST(ErrorInfoTest, ToJsonFromJsonRoundTripsForEveryCategory) {
    for (const auto& c : kAllCategories) {
        const ErrorInfo original =
            ErrorInfo::Make("E_CODE", c.category, "message", "details", c.category == ErrorCategory::NetworkError);

        const ErrorInfo restored = ErrorInfo::FromJson(original.ToJson());

        EXPECT_EQ(restored.code, original.code);
        EXPECT_EQ(restored.category, original.category);
        EXPECT_EQ(restored.message, original.message);
        EXPECT_EQ(restored.details, original.details);
        EXPECT_EQ(restored.recoverable, original.recoverable);
    }
}

TEST(ErrorInfoTest, MakeDefaultsDetailsAndRecoverable) {
    const ErrorInfo info = ErrorInfo::Make("E_CODE", ErrorCategory::Unknown, "message");

    EXPECT_EQ(info.details, "");
    EXPECT_FALSE(info.recoverable);
}

}  // namespace
}  // namespace mediatool::errors
