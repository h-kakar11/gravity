#include "core/jobs/DownloadJob.h"

#include <gtest/gtest.h>

#include <algorithm>

#include "core/downloads/MockDownloadProvider.h"
#include "core/errors/MediaToolException.h"
#include "core/filesystem/MockFileSystem.h"
#include "core/filesystem/PathUtils.h"
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

    mediatool::filesystem::FilenameReservationRegistry registry;
    DownloadJob job(MakeOptions(), provider, fs, /*mediaEngine=*/nullptr, registry);
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

    mediatool::filesystem::FilenameReservationRegistry registry;
    DownloadJob job(MakeOptions(), provider, fs, nullptr, registry);
    job.MarkStarting();
    job.MarkRunning();

    EXPECT_THROW(job.Execute(), MediaToolException);

    ASSERT_TRUE(provider.lastDownloadOptions.has_value());
    EXPECT_EQ(provider.lastDownloadOptions->filenameBase, "My Video (1)");
}

TEST(DownloadJob, DownloadFailureThrowsAndCleansUpArtifacts) {
    MockDownloadProvider provider;
    provider.inspectResult.title = "Broken Video";
    provider.downloadError = ErrorInfo::Make("E_NETWORK", ErrorCategory::NetworkError, "boom");

    MockFileSystem fs;
    fs.AddDirectory("C:\\out");
    FileInfo partial;
    partial.path = "C:\\out\\Broken Video.mp4.part";
    partial.filename = "Broken Video.mp4.part";
    fs.AddFile(partial);

    mediatool::filesystem::FilenameReservationRegistry registry;
    DownloadJob job(MakeOptions(), provider, fs, nullptr, registry);
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

TEST(DownloadJob, DownloadFailureDoesNotDeleteUnrelatedPreExistingFilesOrDirectories) {
    // Regression test for the confirmed data-loss defect: a failed/cancelled download
    // must never delete pre-existing user files or directories that merely share a name
    // prefix with the sanitized job title. Paths are built via paths::Join (as the
    // production code itself does) rather than hardcoded backslash literals, so the test
    // isn't tripped up by std::filesystem's platform-dependent preferred separator.
    MockDownloadProvider provider;
    provider.inspectResult.title = "Clip";
    provider.downloadError = ErrorInfo::Make("E_NETWORK", ErrorCategory::NetworkError, "boom");

    const std::string outDir = MakeOptions().outputDirectory;
    const std::string backupDir = mediatool::filesystem::paths::Join(outDir, "Clip Backup");
    const std::string nestedPath = mediatool::filesystem::paths::Join(backupDir, "important.txt");
    const std::string unrelatedPath = mediatool::filesystem::paths::Join(outDir, "Clip Notes.txt");
    const std::string partialPath = mediatool::filesystem::paths::Join(outDir, "Clip.mp4.part");

    MockFileSystem fs;
    fs.AddDirectory(outDir);

    // Pre-existing, completely unrelated directory whose name is a prefix-superset of
    // the job's sanitized title ("Clip"), with a nested file inside -- exactly the
    // reproduction scenario from the audit (title "Vacation" alongside a pre-existing
    // "Vacation Photos.zip"/directory).
    fs.AddDirectory(backupDir);
    FileInfo nested;
    nested.path = nestedPath;
    nested.filename = "important.txt";
    nested.sizeBytes = 42;
    fs.AddFile(nested);

    // A second unrelated pre-existing file, also a bare prefix match but not a job
    // artifact (no '.' boundary right after the base name).
    FileInfo unrelatedFile;
    unrelatedFile.path = unrelatedPath;
    unrelatedFile.filename = "Clip Notes.txt";
    unrelatedFile.sizeBytes = 7;
    fs.AddFile(unrelatedFile);

    // The job's own partial-download artifact, which cleanup *should* remove.
    FileInfo partial;
    partial.path = partialPath;
    partial.filename = "Clip.mp4.part";
    fs.AddFile(partial);

    mediatool::filesystem::FilenameReservationRegistry registry;
    DownloadJob job(MakeOptions(), provider, fs, nullptr, registry);
    job.MarkStarting();
    job.MarkRunning();

    EXPECT_THROW(job.Execute(), MediaToolException);

    // Pre-existing directory and its nested file must survive.
    EXPECT_TRUE(fs.Exists(backupDir));
    EXPECT_TRUE(fs.Exists(nestedPath));
    // Pre-existing unrelated file must survive.
    EXPECT_TRUE(fs.Exists(unrelatedPath));
    // The job's own artifact must be gone.
    EXPECT_FALSE(fs.Exists(partialPath));
}

TEST(DownloadJob, MissingOutputAfterCompletionFails) {
    MockDownloadProvider provider;
    provider.inspectResult.title = "Ghost Video";
    provider.completedOutputPath = "C:\\out\\Ghost Video.mp4";  // never actually added to fs

    MockFileSystem fs;
    fs.AddDirectory("C:\\out");

    mediatool::filesystem::FilenameReservationRegistry registry;
    DownloadJob job(MakeOptions(), provider, fs, nullptr, registry);
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

    mediatool::filesystem::FilenameReservationRegistry registry;
    DownloadJob job(MakeOptions(), provider, fs, nullptr, registry);
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

    mediatool::filesystem::FilenameReservationRegistry registry;
    DownloadJob job(MakeOptions(), provider, fs, nullptr, registry);
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

    mediatool::filesystem::FilenameReservationRegistry registry;
    DownloadJob job(MakeOptions(), provider, fs, nullptr, registry);
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
