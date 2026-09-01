#include "core/media/DeferredOperations.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "core/errors/ErrorInfo.h"
#include "core/errors/MediaToolException.h"
#include "core/filesystem/FileInfo.h"
#include "core/media/MockMediaEngine.h"

namespace {

using mediatool::errors::ErrorCategory;
using mediatool::errors::MediaToolException;
using mediatool::filesystem::CapabilitiesFor;
using mediatool::filesystem::DeferredCapabilitiesFor;
using mediatool::filesystem::DeferredCapability;
using mediatool::filesystem::FileCategory;
using mediatool::media::DeferralReason;
using mediatool::media::DeferredOperations;
using mediatool::media::IsDeferredOperation;
using mediatool::media::kNotImplementedErrorCode;
using mediatool::media::MakeNotImplementedError;

bool Contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool ContainsCapability(const std::vector<DeferredCapability>& values, const std::string& value) {
    return std::any_of(values.begin(), values.end(),
                        [&](const DeferredCapability& c) { return c.capability == value; });
}

}  // namespace

TEST(DeferredOperations, EveryDeferredOperationCarriesAUserFacingReason) {
    ASSERT_FALSE(DeferredOperations().empty());
    for (const std::string& operation : DeferredOperations()) {
        EXPECT_TRUE(IsDeferredOperation(operation)) << operation;
        // A deferral with no reason is worse than no deferral: the frontend disables a
        // control and cannot say why. The table makes that impossible by construction --
        // this asserts the table stays that way.
        EXPECT_FALSE(DeferralReason(operation).empty()) << operation;
    }
}

TEST(DeferredOperations, AnOperationThatIsNotDeferredHasNoReason) {
    EXPECT_FALSE(IsDeferredOperation("convert"));
    EXPECT_FALSE(IsDeferredOperation("compress"));
    EXPECT_TRUE(DeferralReason("convert").empty());
}

TEST(DeferredOperations, TheNotImplementedErrorIsOneShapeEveryLayerCanMatchOn) {
    const auto error = MakeNotImplementedError(mediatool::media::kExtractAudioOperation);
    EXPECT_EQ(error.code, kNotImplementedErrorCode);
    EXPECT_EQ(error.category, ErrorCategory::UnsupportedFormat);
    // Never recoverable: retrying a deferred operation can only fail again, and Phase C's
    // retry policy keys off exactly this flag.
    EXPECT_FALSE(error.recoverable);
    // The user-facing message IS the deferral reason -- not a second phrasing that could
    // drift away from what the capability list already told the frontend.
    EXPECT_EQ(error.message, DeferralReason(mediatool::media::kExtractAudioOperation));
}

TEST(DeferredOperations, AnUnknownOperationStillGetsTheSameErrorShape) {
    // An engine method nobody has written yet must not need its own spelling of
    // "not implemented" -- callers match one code, always.
    const auto error = MakeNotImplementedError("someFutureOperation");
    EXPECT_EQ(error.code, kNotImplementedErrorCode);
    EXPECT_EQ(error.category, ErrorCategory::UnsupportedFormat);
    EXPECT_FALSE(error.message.empty());
}

TEST(DeferredOperations, VideoCapabilitiesAdvertiseOnlyWhatCanActuallyRun) {
    const std::vector<std::string> capabilities = CapabilitiesFor(FileCategory::Video, "mp4");
    const std::vector<DeferredCapability> deferred =
        DeferredCapabilitiesFor(FileCategory::Video, "mp4");

    EXPECT_TRUE(Contains(capabilities, "convert"));
    EXPECT_TRUE(Contains(capabilities, "compress"));
    // The regression this whole split exists for: these used to be advertised as ordinary
    // capabilities for every video, so the only way to learn they do not run was to start
    // a job and read E_NOT_IMPLEMENTED out of the failure.
    EXPECT_FALSE(Contains(capabilities, "extractAudio"));
    EXPECT_FALSE(Contains(capabilities, "extractFrames"));
    EXPECT_TRUE(ContainsCapability(deferred, "extractAudio"));
    EXPECT_TRUE(ContainsCapability(deferred, "extractFrames"));
}

TEST(DeferredOperations, NoCapabilityIsBothOfferedAndDeferred) {
    // Both directions of the contract in one assertion, across the whole vocabulary: if a
    // token ever appears in both lists the frontend has no way to decide what to do with
    // it.
    const FileCategory categories[] = {FileCategory::Video,   FileCategory::Audio,
                                        FileCategory::Image,   FileCategory::Document,
                                        FileCategory::Text,    FileCategory::Archive,
                                        FileCategory::Unknown};
    for (const FileCategory category : categories) {
        const std::vector<std::string> capabilities = CapabilitiesFor(category, "mp4");
        for (const DeferredCapability& deferred : DeferredCapabilitiesFor(category, "mp4")) {
            EXPECT_FALSE(Contains(capabilities, deferred.capability))
                << "capability " << deferred.capability << " is both offered and deferred";
            EXPECT_FALSE(deferred.reason.empty()) << deferred.capability;
        }
    }
}

TEST(DeferredOperations, OnlyVideoReportsDeferredCapabilities) {
    // Reporting "extractFrames is deferred" for a .zip would be a second kind of lie:
    // implementing it would not make the operation appear there.
    EXPECT_TRUE(DeferredCapabilitiesFor(FileCategory::Audio, "mp3").empty());
    EXPECT_TRUE(DeferredCapabilitiesFor(FileCategory::Image, "png").empty());
    EXPECT_TRUE(DeferredCapabilitiesFor(FileCategory::Archive, "zip").empty());
    EXPECT_FALSE(DeferredCapabilitiesFor(FileCategory::Video, "mp4").empty());
}

TEST(DeferredOperations, DeferredCapabilityJsonCarriesBothFields) {
    const auto json = DeferredCapabilitiesFor(FileCategory::Video, "mp4").front().ToJson();
    EXPECT_TRUE(json.contains("capability"));
    EXPECT_TRUE(json.contains("reason"));
    EXPECT_FALSE(json.at("reason").get<std::string>().empty());
}

TEST(DeferredOperations, TheMockEngineFailsExactlyTheWayTheRealOneDoes) {
    // A test double that reports a different error than the shipped engine lets a passing
    // test coexist with a broken contract.
    mediatool::media::MockMediaEngine engine;
    auto noopProgress = [](const mediatool::jobs::Progress&) {};
    auto neverCancelled = []() { return false; };
    try {
        engine.ExtractFrames("in.mp4", "out_dir", {}, noopProgress, neverCancelled);
        FAIL() << "expected MediaToolException";
    } catch (const MediaToolException& ex) {
        EXPECT_EQ(ex.Info().code, kNotImplementedErrorCode);
        EXPECT_EQ(ex.Info().message,
                   DeferralReason(mediatool::media::kExtractFramesOperation));
    }
}
