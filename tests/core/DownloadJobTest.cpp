#include "core/jobs/DownloadJob.h"

#include <gtest/gtest.h>

#include "tests/support/PlatformTest.h"

#include <algorithm>

#include "core/downloads/MockDownloadProvider.h"
#include "core/errors/MediaToolException.h"
#include "core/filesystem/MockFileSystem.h"
#include "core/jobs/JobTypes.h"

using mediatool::downloads::MockDownloadProvider;
using mediatool::downloads::QualityPreset;
using mediatool::errors::ErrorCategory;
using mediatool::errors::ErrorInfo;
using mediatool::errors::MediaToolException;
using mediatool::filesystem::FileInfo;
using mediatool::filesystem::MockFileSystem;
using mediatool::jobs::DownloadJob;
using mediatool::jobs::JobState;

namespace {

DownloadJob::Options MakeOptions() {
    DownloadJob::Options options;
    options.url = "https://example.com/watch?v=abc123";
    options.outputDirectory = "C:\\out";
    options.quality = QualityPreset::Best;
    return options;
}

}  // namespace

TEST(DownloadJob, CompletesAndVerifiesOutputWithoutMediaEngine) {
    MockDownloadProvider provider;
    provider.inspectResult.title = "My Video";
    provider.completedOutputPath = "C:\\out\\My Video.mp4";

    MockFileSystem fs;
    fs.AddDirectory("C:\\out");
    // Registered only once the (mock) download "starts" -- i.e. after DownloadJob has
    // already computed filenameBase via DeduplicateBaseName -- so it can't be mistaken
    // for a pre-existing collision by that earlier check. See onDownloadStart's comment.
    provider.onDownloadStart = [&fs](const mediatool::downloads::DownloadOptions&) {
        FileInfo outputInfo;
        outputInfo.path = "C:\\out\\My Video.mp4";
        outputInfo.filename = "My Video.mp4";
        outputInfo.extension = "mp4";
        outputInfo.sizeBytes = 12345;
        fs.AddFile(outputInfo);
    };

    DownloadJob job(MakeOptions(), provider, fs, /*mediaEngine=*/nullptr);
    job.MarkStarting();
    job.MarkRunning();
    job.Execute();
    job.MarkCompleted();

    EXPECT_EQ(job.State(), JobState::Completed);
    ASSERT_TRUE(job.GetResult().has_value());
    EXPECT_EQ(job.GetResult()->at("outputPath"), "C:\\out\\My Video.mp4");

    ASSERT_TRUE(provider.lastDownloadOptions.has_value());
    EXPECT_EQ(provider.lastDownloadOptions->filenameBase, "My Video");

    const auto metadata = job.GetMetadata();
    EXPECT_EQ(metadata.at("title"), "My Video");
}

TEST(DownloadJob, DeduplicatesFilenameWhenBaseNameAlreadyExists) {
    SKIP_UNLESS_WINDOWS();
    MockDownloadProvider provider;
    provider.inspectResult.title = "My Video";
    // Never registered in `fs` -- verification will fail with E_DOWNLOAD_OUTPUT_MISSING,
    // which is fine: this test only cares about the filenameBase DownloadJob computed
    // and handed to the provider *before* that, not about the job completing.
    provider.completedOutputPath = "C:\\out\\My Video (1).mp4";

    MockFileSystem fs;
    fs.AddDirectory("C:\\out");
    FileInfo existing;
    existing.path = "C:\\out\\My Video.mp4";
    existing.filename = "My Video.mp4";
    fs.AddFile(existing);  // pre-existing, unrelated file with the same base name

    DownloadJob job(MakeOptions(), provider, fs, nullptr);
    job.MarkStarting();
    job.MarkRunning();

    EXPECT_THROW(job.Execute(), MediaToolException);

    ASSERT_TRUE(provider.lastDownloadOptions.has_value());
    EXPECT_EQ(provider.lastDownloadOptions->filenameBase, "My Video (1)");
}

TEST(DownloadJob, DownloadFailureThrowsAndCleansUpArtifacts) {
    SKIP_UNLESS_WINDOWS();
    MockDownloadProvider provider;
    provider.inspectResult.title = "Broken Video";
    provider.downloadError = ErrorInfo::Make("E_NETWORK", ErrorCategory::NetworkError, "boom");

    MockFileSystem fs;
    fs.AddDirectory("C:\\out");
    FileInfo partial;
    partial.path = "C:\\out\\Broken Video.mp4.part";
    partial.filename = "Broken Video.mp4.part";
    fs.AddFile(partial);

    DownloadJob job(MakeOptions(), provider, fs, nullptr);
    job.MarkStarting();
    job.MarkRunning();

    bool threw = false;
    try {
        job.Execute();
    } catch (const MediaToolException& e) {
        threw = true;
        EXPECT_EQ(e.Info().category, ErrorCategory::NetworkError);
    }
    EXPECT_TRUE(threw);

    const auto& deleted = fs.DeletedPaths();
    EXPECT_NE(std::find(deleted.begin(), deleted.end(), "C:\\out\\Broken Video.mp4.part"), deleted.end());
}

TEST(DownloadJob, MissingOutputAfterCompletionFails) {
    MockDownloadProvider provider;
    provider.inspectResult.title = "Ghost Video";
    provider.completedOutputPath = "C:\\out\\Ghost Video.mp4";  // never actually added to fs

    MockFileSystem fs;
    fs.AddDirectory("C:\\out");

    DownloadJob job(MakeOptions(), provider, fs, nullptr);
    job.MarkStarting();
    job.MarkRunning();

    bool threw = false;
    try {
        job.Execute();
    } catch (const MediaToolException& e) {
        threw = true;
        EXPECT_EQ(e.Info().code, "E_DOWNLOAD_OUTPUT_MISSING");
    }
    EXPECT_TRUE(threw);
}

TEST(DownloadJob, EmptyOutputFileFails) {
    MockDownloadProvider provider;
    provider.inspectResult.title = "Empty Video";
    provider.completedOutputPath = "C:\\out\\Empty Video.mp4";

    MockFileSystem fs;
    fs.AddDirectory("C:\\out");
    FileInfo empty;
    empty.path = "C:\\out\\Empty Video.mp4";
    empty.filename = "Empty Video.mp4";
    empty.sizeBytes = 0;
    fs.AddFile(empty);

    DownloadJob job(MakeOptions(), provider, fs, nullptr);
    job.MarkStarting();
    job.MarkRunning();

    bool threw = false;
    try {
        job.Execute();
    } catch (const MediaToolException& e) {
        threw = true;
        EXPECT_EQ(e.Info().code, "E_DOWNLOAD_OUTPUT_EMPTY");
    }
    EXPECT_TRUE(threw);
}

TEST(DownloadJob, InspectFailurePropagatesAsCancelled) {
    MockDownloadProvider provider;
    provider.inspectError =
        ErrorInfo::Make("E_INSPECT_CANCELLED", ErrorCategory::Cancelled, "cancelled", "", true);

    MockFileSystem fs;
    fs.AddDirectory("C:\\out");

    DownloadJob job(MakeOptions(), provider, fs, nullptr);
    job.MarkStarting();
    job.MarkRunning();

    bool threw = false;
    try {
        job.Execute();
    } catch (const MediaToolException& e) {
        threw = true;
        EXPECT_EQ(e.Info().category, ErrorCategory::Cancelled);
    }
    EXPECT_TRUE(threw);
}

TEST(DownloadJob, PlaylistUrlSurfacesAsUnsupportedFormat) {
    MockDownloadProvider provider;
    provider.inspectError = ErrorInfo::Make("E_PLAYLIST_NOT_SUPPORTED", ErrorCategory::UnsupportedFormat,
                                             "This URL is a playlist.");

    MockFileSystem fs;
    fs.AddDirectory("C:\\out");

    DownloadJob job(MakeOptions(), provider, fs, nullptr);
    job.MarkStarting();
    job.MarkRunning();

    bool threw = false;
    try {
        job.Execute();
    } catch (const MediaToolException& e) {
        threw = true;
        EXPECT_EQ(e.Info().code, "E_PLAYLIST_NOT_SUPPORTED");
        EXPECT_EQ(e.Info().category, ErrorCategory::UnsupportedFormat);
    }
    EXPECT_TRUE(threw);
}
