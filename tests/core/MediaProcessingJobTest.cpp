#include "core/jobs/MediaProcessingJob.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <stdexcept>
#include <utility>

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

// --- issue #80: compression must have a size objective, not just a quality one ----------

namespace {

// Builds a job whose input probes at `sourceBitrateBps` and runs it to completion,
// returning the options the engine actually received.
nlohmann::json RunAndCaptureEngineOptions(bool isCompression, const std::string& outputFormat,
                                          std::int64_t sourceBitrateBps,
                                          const nlohmann::json& extraOptions = nlohmann::json::object()) {
    const std::string inputPath = paths::Join("C:\\in", "clip.mp4");
    const std::string outDir = "C:\\out";

    MockFileSystem fs;
    fs.AddDirectory("C:\\in");
    fs.AddDirectory(outDir);
    FileInfo input;
    input.path = inputPath;
    input.filename = "clip.mp4";
    input.sizeBytes = 1219022;
    fs.AddFile(input);

    MockMediaEngine engine;
    engine.probeResult.bitrate = sourceBitrateBps;
    engine.onProcessingStart = [&fs](const std::string& outputPath) {
        FileInfo out;
        out.path = outputPath;
        out.filename = paths::GetFilename(outputPath);
        out.sizeBytes = 512;
        fs.AddFile(out);
    };

    MediaProcessingJob::Options options = MakeOptions(inputPath, outDir, outputFormat);
    options.isCompression = isCompression;
    for (auto it = extraOptions.begin(); it != extraOptions.end(); ++it) {
        options.engineOptions[it.key()] = it.value();
    }

    FilenameReservationRegistry registry;
    MediaProcessingJob job(std::move(options), engine, fs, registry);
    job.MarkStarting();
    job.MarkRunning();
    job.Execute();

    EXPECT_TRUE(engine.lastOptions.has_value());
    return engine.lastOptions.value_or(nlohmann::json::object());
}

}  // namespace

TEST(MediaProcessingJob, CompressionDerivesAVideoBitrateTargetFromTheProbedSource) {
    // Issue #80: "compress" used to be byte-for-byte the same ffmpeg invocation as
    // "convert" -- a CRF quality target with no notion of the input's size at all, which
    // is why re-encoding an already-compressed 1.19 MB clip came back at 1.03x (medium)
    // and 1.41x (high). The job now probes the input first and hands the engine a bitrate
    // derived from what the source actually is.
    const auto options = RunAndCaptureEngineOptions(/*isCompression=*/true, "mp4",
                                                     /*sourceBitrateBps=*/750'000);
    ASSERT_TRUE(options.contains("videoBitrateKbps"));
    // 750 kbps source, "medium" compression factor 0.45 => 338 kbps overall, less the
    // 128 kbps audio budget the same call reserves.
    EXPECT_EQ(options.at("videoBitrateKbps").get<int>(), 338 - 128);
    EXPECT_EQ(options.at("audioBitrateKbps").get<int>(), 128);
}

TEST(MediaProcessingJob, EveryCompressionQualityTierTargetsLessThanTheSource) {
    // The property the issue is actually about: whatever tier the user picks, the total
    // target has to come in under the source. A quality-only knob could not promise this.
    for (const std::string quality : {"lowest", "low", "medium", "high", "ultra"}) {
        const auto options = RunAndCaptureEngineOptions(
            /*isCompression=*/true, "mp4", /*sourceBitrateBps=*/2'000'000, {{"quality", quality}});
        ASSERT_TRUE(options.contains("videoBitrateKbps")) << "quality=" << quality;
        const int total =
            options.at("videoBitrateKbps").get<int>() + options.at("audioBitrateKbps").get<int>();
        EXPECT_LT(total, 2000) << "quality=" << quality;
    }
}

TEST(MediaProcessingJob, AudioOnlyCompressionSizesTheAudioStreamInstead) {
    // An mp3/wav target has no video stream for -b:v to apply to; the audio bitrate is
    // the only size lever there is.
    const auto options = RunAndCaptureEngineOptions(/*isCompression=*/true, "mp3",
                                                     /*sourceBitrateBps=*/320'000);
    EXPECT_FALSE(options.contains("videoBitrateKbps"));
    ASSERT_TRUE(options.contains("audioBitrateKbps"));
    EXPECT_LT(options.at("audioBitrateKbps").get<int>(), 320);
}

TEST(MediaProcessingJob, AnExplicitAudioBitrateIsNeverOverriddenBySizing) {
    // The user asked for a specific bitrate, not a ratio of the source.
    const auto options = RunAndCaptureEngineOptions(
        /*isCompression=*/true, "mp3", /*sourceBitrateBps=*/320'000, {{"audioBitrateKbps", 192}});
    EXPECT_EQ(options.at("audioBitrateKbps").get<int>(), 192);
}

TEST(MediaProcessingJob, AnUnprobableSourceLeavesTheEngineOptionsUntouched) {
    // No bitrate reported means no basis for a target. Inventing one from a number we
    // don't have would be worse than leaving the engine on its previous behavior.
    const auto options = RunAndCaptureEngineOptions(/*isCompression=*/true, "mp4",
                                                     /*sourceBitrateBps=*/0);
    EXPECT_FALSE(options.contains("videoBitrateKbps"));
}

TEST(MediaProcessingJob, ConversionDoesNotShrinkTheWayCompressionDoes) {
    // Same machinery, different intent: a conversion is only asked not to balloon.
    const auto compress = RunAndCaptureEngineOptions(/*isCompression=*/true, "mp4", 2'000'000);
    const auto convert = RunAndCaptureEngineOptions(/*isCompression=*/false, "mp4", 2'000'000);
    ASSERT_TRUE(compress.contains("videoBitrateKbps") && convert.contains("videoBitrateKbps"));
    EXPECT_LT(compress.at("videoBitrateKbps").get<int>(), convert.at("videoBitrateKbps").get<int>());
}

TEST(MediaProcessingJob, ReportsItsArtifactLocationAsSoonAsItReservesAName) {
    // The one fact only a run knows. A killed process leaves a half-written file whose
    // name is derived on the worker thread; without this hook the recovery pass has no way
    // to scope a cleanup to it, and the file survives forever.
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
    const std::string expectedOutput = paths::Join(outDir, "Vacation Clip.mp4");
    engine.onProcessingStart = [&fs, expectedOutput](const std::string& outputPath) {
        FileInfo out;
        out.path = outputPath;
        out.filename = paths::GetFilename(outputPath);
        out.sizeBytes = 512;
        fs.AddFile(out);
    };

    std::string reportedDirectory;
    std::string reportedBase;
    int callCount = 0;

    FilenameReservationRegistry registry;
    auto options = MakeOptions(inputPath, outDir, "mp4");
    options.onArtifactLocation = [&](const std::string& directory, const std::string& base) {
        reportedDirectory = directory;
        reportedBase = base;
        ++callCount;
    };
    MediaProcessingJob job(std::move(options), engine, fs, registry);
    job.Execute();

    EXPECT_EQ(callCount, 1);
    EXPECT_EQ(reportedDirectory, outDir);
    // The RESERVED base, not the raw input stem -- a second job converting a file of the
    // same name gets "Vacation Clip (1)", and cleaning up the wrong one would delete a
    // different job's output.
    EXPECT_EQ(reportedBase, "Vacation Clip");
}

TEST(MediaProcessingJob, CleansUpEvenWhenTheEngineThrowsSomethingOtherThanAMediaToolException) {
    // The cleanup handler used to be catch(MediaToolException&), so a bad_alloc, a json
    // exception, or anything else escaping the engine skipped cleanup entirely -- the one
    // case where a half-written file outlives every code path that knows its name.
    const std::string inputPath = paths::Join("C:\\in", "Clip.mov");
    const std::string outDir = "C:\\out";

    MockFileSystem fs;
    fs.AddDirectory("C:\\in");
    fs.AddDirectory(outDir);
    FileInfo input;
    input.path = inputPath;
    input.filename = "Clip.mov";
    input.sizeBytes = 999;
    fs.AddFile(input);

    MockMediaEngine engine;
    engine.onProcessingStart = [&fs](const std::string& outputPath) {
        FileInfo partial;
        partial.path = outputPath;
        partial.filename = paths::GetFilename(outputPath);
        partial.sizeBytes = 128;  // a partial encode, already on disk
        fs.AddFile(partial);
        throw std::runtime_error("engine blew up in a way nobody classified");
    };

    FilenameReservationRegistry registry;
    MediaProcessingJob job(MakeOptions(inputPath, outDir, "mp4"), engine, fs, registry);

    EXPECT_THROW(job.Execute(), std::runtime_error);
    EXPECT_FALSE(fs.Exists(paths::Join(outDir, "Clip.mp4")));
}

