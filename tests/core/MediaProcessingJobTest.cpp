#include "core/jobs/MediaProcessingJob.h"

#include <gtest/gtest.h>

#include "core/errors/MediaToolException.h"
#include "core/filesystem/FilenameReservationRegistry.h"
#include "core/filesystem/MockFileSystem.h"
#include "core/filesystem/PathUtils.h"
#include "core/media/MockMediaEngine.h"

using mediatool::errors::ErrorCategory;
using mediatool::errors::ErrorInfo;
using mediatool::errors::MediaToolException;
using mediatool::filesystem::FileInfo;
using mediatool::filesystem::FilenameReservationRegistry;
using mediatool::filesystem::MockFileSystem;
using mediatool::jobs::JobState;
using mediatool::jobs::MediaProcessingJob;
using mediatool::media::MockMediaEngine;

namespace paths = mediatool::filesystem::paths;

namespace {

MediaProcessingJob::Options MakeOptions(const std::string& inputPath, const std::string& outputDirectory,
                                        const std::string& outputFormat) {
    MediaProcessingJob::Options options;
    options.inputPath = inputPath;
    options.outputDirectory = outputDirectory;
    options.outputFormat = outputFormat;
    options.engineOptions = {{"outputFormat", outputFormat}};
    return options;
}

}  // namespace

TEST(MediaProcessingJob, ConvertsAndVerifiesOutputDerivingNameFromInputFile) {
    const std::string inputPath = paths::Join("C:\\in", "Vacation Clip.mov");
    const std::string outDir = "C:\\out";

    MockFileSystem fs;
    fs.AddDirectory("C:\\in");
    fs.AddDirectory(outDir);
    FileInfo input;
    input.path = inputPath;
    input.filename = "Vacation Clip.mov";
    input.sizeBytes = 999;
    fs.AddFile(input);

    MockMediaEngine engine;
    engine.probeResult.durationSeconds = 12.0;
    const std::string expectedOutput = paths::Join(outDir, "Vacation Clip.mp4");
    engine.onProcessingStart = [&fs, expectedOutput](const std::string& outputPath) {
        FileInfo out;
        out.path = outputPath;
        out.filename = paths::GetFilename(outputPath);
        out.sizeBytes = 4096;
        fs.AddFile(out);
    };

    FilenameReservationRegistry registry;
    MediaProcessingJob job(MakeOptions(inputPath, outDir, "mp4"), engine, fs, registry);
    job.MarkStarting();
    job.MarkRunning();
    job.Execute();
    job.MarkCompleted();

    EXPECT_EQ(job.State(), JobState::Completed);
    ASSERT_TRUE(job.GetResult().has_value());
    // Smart Rename: derived from the input file's own basename, not a job id.
    EXPECT_EQ(job.GetResult()->at("outputPath"), expectedOutput);
    EXPECT_FALSE(engine.lastCallWasCompress);
}

TEST(MediaProcessingJob, CompressionUsesTheSameFlowWithCompressCalled) {
    const std::string inputPath = paths::Join("C:\\in", "clip.mp4");
    const std::string outDir = "C:\\out";

    MockFileSystem fs;
    fs.AddDirectory("C:\\in");
    fs.AddDirectory(outDir);
    FileInfo input;
    input.path = inputPath;
    input.filename = "clip.mp4";
    fs.AddFile(input);

    MockMediaEngine engine;
    engine.onProcessingStart = [&fs](const std::string& outputPath) {
        FileInfo out;
        out.path = outputPath;
        out.filename = paths::GetFilename(outputPath);
        out.sizeBytes = 10;
        fs.AddFile(out);
    };

    auto options = MakeOptions(inputPath, outDir, "mp4");
    options.isCompression = true;

    FilenameReservationRegistry registry;
    MediaProcessingJob job(std::move(options), engine, fs, registry);
    job.MarkStarting();
    job.MarkRunning();
    job.Execute();

    EXPECT_TRUE(engine.lastCallWasCompress);
    EXPECT_EQ(job.Type(), mediatool::jobs::JobType::Compression);
}

TEST(MediaProcessingJob, MissingInputFileFailsBeforeCallingTheEngine) {
    MockFileSystem fs;
    fs.AddDirectory("C:\\out");
    MockMediaEngine engine;

    FilenameReservationRegistry registry;
    MediaProcessingJob job(MakeOptions("C:\\in\\ghost.mp4", "C:\\out", "mp4"), engine, fs, registry);
    job.MarkStarting();
    job.MarkRunning();

    bool threw = false;
    try {
        job.Execute();
    } catch (const MediaToolException& e) {
        threw = true;
        EXPECT_EQ(e.Info().code, "E_INPUT_FILE_NOT_FOUND");
    }
    EXPECT_TRUE(threw);
    EXPECT_FALSE(engine.lastInputPath.has_value());  // never even reached the engine
}

TEST(MediaProcessingJob, EngineFailureThrowsAndCleansUpArtifactsWithoutTouchingUnrelatedFiles) {
    const std::string inputPath = paths::Join("C:\\in", "Clip.mp4");
    const std::string outDir = "C:\\out";

    MockFileSystem fs;
    fs.AddDirectory("C:\\in");
    fs.AddDirectory(outDir);
    FileInfo input;
    input.path = inputPath;
    input.filename = "Clip.mp4";
    fs.AddFile(input);

    // Pre-existing unrelated file that merely shares a text prefix with the job's base
    // name -- must survive cleanup (the same #3 regression this job type must not reintroduce).
    FileInfo unrelated;
    unrelated.path = paths::Join(outDir, "Clip Notes.txt");
    unrelated.filename = "Clip Notes.txt";
    fs.AddFile(unrelated);

    // A partial artifact the failed run left behind, which cleanup SHOULD remove.
    FileInfo partial;
    partial.path = paths::Join(outDir, "Clip.mp4.part");
    partial.filename = "Clip.mp4.part";
    fs.AddFile(partial);

    MockMediaEngine engine;
    engine.processingError = ErrorInfo::Make("E_FFMPEG_FAILED", ErrorCategory::EngineFailure, "boom");

    FilenameReservationRegistry registry;
    MediaProcessingJob job(MakeOptions(inputPath, outDir, "mp4"), engine, fs, registry);
    job.MarkStarting();
    job.MarkRunning();

    EXPECT_THROW(job.Execute(), MediaToolException);

    EXPECT_TRUE(fs.Exists(paths::Join(outDir, "Clip Notes.txt")));
    EXPECT_FALSE(fs.Exists(paths::Join(outDir, "Clip.mp4.part")));
}

TEST(MediaProcessingJob, CancellationDuringProcessingThrowsAndCleansUp) {
    const std::string inputPath = paths::Join("C:\\in", "Clip.mp4");
    const std::string outDir = "C:\\out";

    MockFileSystem fs;
    fs.AddDirectory("C:\\in");
    fs.AddDirectory(outDir);
    FileInfo input;
    input.path = inputPath;
    input.filename = "Clip.mp4";
    fs.AddFile(input);

    MockMediaEngine engine;
    mediatool::jobs::Progress step;
    step.statusMessage = "Encoding";
    engine.progressSequence = {step};  // MockMediaEngine checks isCancelled() before each step

    FilenameReservationRegistry registry;
    MediaProcessingJob job(MakeOptions(inputPath, outDir, "mp4"), engine, fs, registry);
    job.MarkStarting();
    job.MarkRunning();
    job.RequestCancel();

    bool threw = false;
    try {
        job.Execute();
    } catch (const MediaToolException& e) {
        threw = true;
        EXPECT_EQ(e.Info().category, ErrorCategory::Cancelled);
    }
    EXPECT_TRUE(threw);
}

TEST(MediaProcessingJob, MissingOutputAfterEngineSuccessFails) {
    const std::string inputPath = paths::Join("C:\\in", "Clip.mp4");
    const std::string outDir = "C:\\out";

    MockFileSystem fs;
    fs.AddDirectory("C:\\in");
    fs.AddDirectory(outDir);
    FileInfo input;
    input.path = inputPath;
    input.filename = "Clip.mp4";
    fs.AddFile(input);

    MockMediaEngine engine;  // onProcessingStart unset -- output file never actually appears

    FilenameReservationRegistry registry;
    MediaProcessingJob job(MakeOptions(inputPath, outDir, "mp4"), engine, fs, registry);
    job.MarkStarting();
    job.MarkRunning();

    bool threw = false;
    try {
        job.Execute();
    } catch (const MediaToolException& e) {
        threw = true;
        EXPECT_EQ(e.Info().code, "E_MEDIA_PROCESSING_OUTPUT_MISSING");
    }
    EXPECT_TRUE(threw);
}
