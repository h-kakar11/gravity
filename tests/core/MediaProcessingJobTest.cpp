// ConversionJob and CompressionJob through their shared MediaProcessingJob lifecycle,
// against mocks -- no ffmpeg process, no real disk. What is asserted here is the wrapper
// around the engine call: input validation, output naming, artifact cleanup between
// attempts, verification of what came back, and cancellation checkpoints.
//
// The real encode is covered separately by FFmpegArgumentBuilderTest (the argv) and by the
// end-to-end run (a real ffmpeg producing a real file).

#include "core/jobs/CompressionJob.h"
#include "core/jobs/ConversionJob.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "core/errors/MediaToolException.h"
#include "core/filesystem/MockFileSystem.h"
#include "core/media/IMediaEngine.h"

using mediatool::errors::ErrorCategory;
using mediatool::errors::ErrorInfo;
using mediatool::errors::MediaToolException;
using mediatool::filesystem::FileInfo;
using mediatool::filesystem::MockFileSystem;
using mediatool::jobs::CompressionJob;
using mediatool::jobs::ConversionJob;
using mediatool::jobs::JobState;
using mediatool::jobs::Progress;
using mediatool::media::CompressionPreset;
using mediatool::media::TargetFormat;

namespace {

// A scripted IMediaEngine. Records what it was asked to do and, by default, "produces" the
// output by registering it with the MockFileSystem -- which is how a job's post-run
// verification gets something to find.
class ScriptedMediaEngine final : public mediatool::media::IMediaEngine {
public:
    explicit ScriptedMediaEngine(MockFileSystem& fileSystem) : fileSystem_(fileSystem) {}

    bool IsAvailable() const override { return available; }
    std::optional<std::string> Version() const override { return "scripted"; }

    FileInfo Probe(const std::string& path) override {
        FileInfo info;
        info.path = path;
        info.durationSeconds = 10.0;
        return info;
    }

    void Convert(const std::string& inputPath, const std::string& outputPath,
                 const nlohmann::json& options, mediatool::media::ProgressCallback onProgress,
                 mediatool::media::CancelledCallback isCancelled) override {
        Run("Convert", inputPath, outputPath, options, onProgress, isCancelled);
    }

    void Compress(const std::string& inputPath, const std::string& outputPath,
                  const nlohmann::json& options, mediatool::media::ProgressCallback onProgress,
                  mediatool::media::CancelledCallback isCancelled) override {
        Run("Compress", inputPath, outputPath, options, onProgress, isCancelled);
    }

    void ExtractAudio(const std::string&, const std::string&, mediatool::media::ProgressCallback,
                      mediatool::media::CancelledCallback) override {}
    void ExtractFrames(const std::string&, const std::string&, const nlohmann::json&,
                       mediatool::media::ProgressCallback,
                       mediatool::media::CancelledCallback) override {}

    // --- scripting ---
    bool available = true;
    std::optional<ErrorInfo> failWith;
    // When true the engine returns successfully but writes nothing, exercising the job's
    // "the engine claimed success but there is no file" check.
    bool produceNothing = false;
    std::uint64_t producedSizeBytes = 1024;

    // --- observation ---
    std::vector<std::string> calls;
    std::string lastInputPath;
    std::string lastOutputPath;
    nlohmann::json lastOptions;
    int invocations = 0;

private:
    void Run(const std::string& name, const std::string& inputPath, const std::string& outputPath,
             const nlohmann::json& options, mediatool::media::ProgressCallback onProgress,
             mediatool::media::CancelledCallback isCancelled) {
        ++invocations;
        calls.push_back(name);
        lastInputPath = inputPath;
        lastOutputPath = outputPath;
        lastOptions = options;

        if (onProgress) onProgress(Progress{.percentage = 50.0, .statusMessage = name});
        if (isCancelled && isCancelled()) {
            throw MediaToolException(
                ErrorInfo::Make("E_FFMPEG_CANCELLED", ErrorCategory::Cancelled, "cancelled"));
        }
        if (failWith) throw MediaToolException(*failWith);
        if (produceNothing) return;

        FileInfo produced;
        produced.path = outputPath;
        produced.filename = outputPath.substr(outputPath.find_last_of("/\\") + 1);
        produced.sizeBytes = producedSizeBytes;
        fileSystem_.AddFile(produced);
    }

    MockFileSystem& fileSystem_;
};

// Registers `path` as an existing file so a job's "does my input still exist" check passes.
void AddExistingFile(MockFileSystem& fs, const std::string& path, std::uint64_t size = 4096) {
    FileInfo info;
    info.path = path;
    info.filename = path.substr(path.find_last_of("/\\") + 1);
    info.sizeBytes = size;
    fs.AddFile(info);
}

ConversionJob::Options MakeConversionOptions(const std::string& input, const std::string& outDir,
                                             TargetFormat format = TargetFormat::Mp3) {
    ConversionJob::Options options;
    options.common.inputPath = input;
    options.common.outputDirectory = outDir;
    options.request.targetFormat = format;
    return options;
}

// Drives a job the way JobManager's worker would.
void RunToCompletion(mediatool::jobs::Job& job) {
    job.MarkStarting();
    job.MarkRunning();
    job.Execute();
    job.MarkCompleted();
}

}  // namespace

// --- happy paths -------------------------------------------------------------------------

TEST(MediaProcessingJob, ConversionRunsTheEngineAndPublishesItsOutput) {
    MockFileSystem fs;
    ScriptedMediaEngine engine(fs);
    fs.AddDirectory("/out");
    AddExistingFile(fs, "/media/clip.mp4");

    ConversionJob job(MakeConversionOptions("/media/clip.mp4", "/out"), engine, fs);
    RunToCompletion(job);

    EXPECT_EQ(job.State(), JobState::Completed);
    EXPECT_EQ(engine.calls, std::vector<std::string>{"Convert"});
    EXPECT_EQ(engine.lastInputPath, "/media/clip.mp4");
    // The output name is derived from the input stem plus the target extension.
    EXPECT_NE(engine.lastOutputPath.find("clip.mp3"), std::string::npos);

    ASSERT_TRUE(job.GetResult().has_value());
    EXPECT_NE((*job.GetResult())["outputPath"].get<std::string>().find("clip.mp3"),
              std::string::npos);
}

TEST(MediaProcessingJob, CompressionRunsTheEngineWithItsPreset) {
    MockFileSystem fs;
    ScriptedMediaEngine engine(fs);
    fs.AddDirectory("/out");
    AddExistingFile(fs, "/media/clip.mp4");

    CompressionJob::Options options;
    options.common.inputPath = "/media/clip.mp4";
    options.common.outputDirectory = "/out";
    options.request.preset = CompressionPreset::High;
    options.request.maxHeight = 720;
    options.outputExtension = "mp4";

    CompressionJob job(options, engine, fs);
    RunToCompletion(job);

    EXPECT_EQ(job.State(), JobState::Completed);
    EXPECT_EQ(engine.calls, std::vector<std::string>{"Compress"});
    EXPECT_EQ(engine.lastOptions["preset"], "HIGH");
    EXPECT_EQ(engine.lastOptions["maxHeight"], 720);
}

TEST(MediaProcessingJob, MetadataDescribesTheOperationWithoutLeakingACommandLine) {
    MockFileSystem fs;
    ScriptedMediaEngine engine(fs);
    fs.AddDirectory("/out");
    AddExistingFile(fs, "/media/holiday.mp4");

    ConversionJob job(MakeConversionOptions("/media/holiday.mp4", "/out", TargetFormat::WebM),
                      engine, fs);
    RunToCompletion(job);

    const auto metadata = job.GetMetadata();
    EXPECT_EQ(metadata["inputFilename"], "holiday.mp4");
    EXPECT_EQ(metadata["sourceFormat"], "mp4");
    EXPECT_EQ(metadata["targetFormat"], "webm");
    EXPECT_EQ(metadata["operation"], "CONVERSION");
    EXPECT_TRUE(metadata.contains("outputFilename"));

    // Spec section 21: the queue describes what a job does, never how ffmpeg is invoked.
    const std::string dumped = metadata.dump();
    EXPECT_EQ(dumped.find("ffmpeg"), std::string::npos);
    EXPECT_EQ(dumped.find("-crf"), std::string::npos);
    EXPECT_EQ(dumped.find("libx264"), std::string::npos);
}

TEST(MediaProcessingJob, ProgressFromTheEngineReachesTheJob) {
    MockFileSystem fs;
    ScriptedMediaEngine engine(fs);
    fs.AddDirectory("/out");
    AddExistingFile(fs, "/media/clip.mp4");

    std::vector<double> percentages;
    ConversionJob job(MakeConversionOptions("/media/clip.mp4", "/out"), engine, fs);
    job.SetCallbacks([](JobState) {}, [&percentages](const Progress& progress) {
        if (progress.percentage) percentages.push_back(*progress.percentage);
    });
    RunToCompletion(job);

    EXPECT_FALSE(percentages.empty());
    EXPECT_EQ(percentages.back(), 50.0);
}

TEST(MediaProcessingJob, OutputNameCollidingWithAnUnrelatedFileIsDeduplicated) {
    MockFileSystem fs;
    ScriptedMediaEngine engine(fs);
    fs.AddDirectory("/out");
    AddExistingFile(fs, "/media/clip.mp4");
    // A file the user already had, which this job must not overwrite.
    AddExistingFile(fs, "/out/clip.mp3");

    ConversionJob job(MakeConversionOptions("/media/clip.mp4", "/out"), engine, fs);
    RunToCompletion(job);

    EXPECT_NE(engine.lastOutputPath.find("clip (1).mp3"), std::string::npos)
        << "chose " << engine.lastOutputPath;
}

TEST(MediaProcessingJob, AnExplicitOutputNameIsUsedWithTheTargetExtension) {
    MockFileSystem fs;
    ScriptedMediaEngine engine(fs);
    fs.AddDirectory("/out");
    AddExistingFile(fs, "/media/clip.mp4");

    auto options = MakeConversionOptions("/media/clip.mp4", "/out", TargetFormat::Flac);
    options.common.outputFilenameBase = "my audio";
    ConversionJob job(options, engine, fs);
    RunToCompletion(job);

    EXPECT_NE(engine.lastOutputPath.find("my audio.flac"), std::string::npos)
        << "chose " << engine.lastOutputPath;
}

// --- input validation ----------------------------------------------------------------------

TEST(MediaProcessingJob, AMissingInputFailsWithoutInvokingTheEngine) {
    MockFileSystem fs;
    ScriptedMediaEngine engine(fs);
    fs.AddDirectory("/out");
    // The input is deliberately never registered -- the user moved or deleted it while the
    // job sat in the queue.

    ConversionJob job(MakeConversionOptions("/media/gone.mp4", "/out"), engine, fs);
    job.MarkStarting();
    job.MarkRunning();

    try {
        job.Execute();
        FAIL() << "expected the job to reject a missing input";
    } catch (const MediaToolException& e) {
        EXPECT_EQ(e.Info().code, "E_INPUT_NOT_FOUND");
        EXPECT_EQ(e.Info().category, ErrorCategory::FileNotFound);
    }
    EXPECT_EQ(engine.invocations, 0);
}

TEST(MediaProcessingJob, AnEmptyInputOrOutputPathIsRejected) {
    MockFileSystem fs;
    ScriptedMediaEngine engine(fs);

    ConversionJob noInput(MakeConversionOptions("", "/out"), engine, fs);
    noInput.MarkStarting();
    noInput.MarkRunning();
    EXPECT_THROW(noInput.Execute(), MediaToolException);

    AddExistingFile(fs, "/media/clip.mp4");
    ConversionJob noOutput(MakeConversionOptions("/media/clip.mp4", ""), engine, fs);
    noOutput.MarkStarting();
    noOutput.MarkRunning();
    EXPECT_THROW(noOutput.Execute(), MediaToolException);
    EXPECT_EQ(engine.invocations, 0);
}

// --- verification --------------------------------------------------------------------------

TEST(MediaProcessingJob, EngineSuccessWithNoFileIsTreatedAsAFailure) {
    // "ffmpeg exited 0" is not proof of a usable file (spec section 27).
    MockFileSystem fs;
    ScriptedMediaEngine engine(fs);
    engine.produceNothing = true;
    fs.AddDirectory("/out");
    AddExistingFile(fs, "/media/clip.mp4");

    ConversionJob job(MakeConversionOptions("/media/clip.mp4", "/out"), engine, fs);
    job.MarkStarting();
    job.MarkRunning();

    try {
        job.Execute();
        FAIL() << "expected a missing-output failure";
    } catch (const MediaToolException& e) {
        EXPECT_EQ(e.Info().code, "E_OUTPUT_MISSING");
    }
}

TEST(MediaProcessingJob, AnEmptyOutputIsRejectedAndDeleted) {
    MockFileSystem fs;
    ScriptedMediaEngine engine(fs);
    engine.producedSizeBytes = 0;
    fs.AddDirectory("/out");
    AddExistingFile(fs, "/media/clip.mp4");

    ConversionJob job(MakeConversionOptions("/media/clip.mp4", "/out"), engine, fs);
    job.MarkStarting();
    job.MarkRunning();

    try {
        job.Execute();
        FAIL() << "expected an empty-output failure";
    } catch (const MediaToolException& e) {
        EXPECT_EQ(e.Info().code, "E_OUTPUT_EMPTY");
    }
    // A zero-byte file must not be left sitting where the user expects real output.
    const auto& deleted = fs.DeletedPaths();
    EXPECT_NE(std::find_if(deleted.begin(), deleted.end(),
                           [](const std::string& path) {
                               return path.find("clip.mp3") != std::string::npos;
                           }),
              deleted.end());
}

TEST(MediaProcessingJob, AnEngineFailurePropagatesItsStructuredError) {
    MockFileSystem fs;
    ScriptedMediaEngine engine(fs);
    engine.failWith = ErrorInfo::Make("E_FFMPEG_FAILED", ErrorCategory::EngineFailure,
                                       "ffmpeg could not convert this file.", "exit code 1");
    fs.AddDirectory("/out");
    AddExistingFile(fs, "/media/clip.mp4");

    ConversionJob job(MakeConversionOptions("/media/clip.mp4", "/out"), engine, fs);
    job.MarkStarting();
    job.MarkRunning();

    try {
        job.Execute();
        FAIL() << "expected the engine failure to propagate";
    } catch (const MediaToolException& e) {
        // The job must not rewrite the engine's diagnosis into something vaguer.
        EXPECT_EQ(e.Info().code, "E_FFMPEG_FAILED");
        EXPECT_EQ(e.Info().details, "exit code 1");
    }
}

// --- cancellation -------------------------------------------------------------------------

TEST(MediaProcessingJob, CancellationBeforeTheEngineRunsSkipsItEntirely) {
    MockFileSystem fs;
    ScriptedMediaEngine engine(fs);
    fs.AddDirectory("/out");
    AddExistingFile(fs, "/media/clip.mp4");

    ConversionJob job(MakeConversionOptions("/media/clip.mp4", "/out"), engine, fs);
    job.MarkStarting();
    job.MarkRunning();
    job.RequestCancel();

    try {
        job.Execute();
        FAIL() << "expected cancellation";
    } catch (const MediaToolException& e) {
        EXPECT_EQ(e.Info().category, ErrorCategory::Cancelled);
    }
    EXPECT_EQ(engine.invocations, 0) << "a cancelled job must not launch the engine";
}

TEST(MediaProcessingJob, TheEnginesCancellationProbeReflectsTheJob) {
    MockFileSystem fs;
    ScriptedMediaEngine engine(fs);
    fs.AddDirectory("/out");
    AddExistingFile(fs, "/media/clip.mp4");

    // The scripted engine reports progress and then checks isCancelled(). Cancelling from
    // *that* report -- rather than from the job's own earlier "Preparing" one, which fires
    // before the engine is reached at all -- proves the probe the engine was handed is wired
    // to this job rather than to a stale copy.
    ConversionJob job(MakeConversionOptions("/media/clip.mp4", "/out"), engine, fs);
    job.SetCallbacks([](JobState) {}, [&job](const Progress& progress) {
        if (progress.statusMessage == "Convert") job.RequestCancel();
    });
    job.MarkStarting();
    job.MarkRunning();

    try {
        job.Execute();
        FAIL() << "expected cancellation";
    } catch (const MediaToolException& e) {
        EXPECT_EQ(e.Info().category, ErrorCategory::Cancelled);
    }
    EXPECT_EQ(engine.invocations, 1);
}

// --- retry cleanup ------------------------------------------------------------------------

TEST(MediaProcessingJob, RetryingReclaimsTheJobsOwnNameInsteadOfAccumulating) {
    // Spec section 17: a job retried three times must not leave "clip (1).mp3",
    // "clip (2).mp3", "clip (3).mp3" behind. Two real attempts of the same job object,
    // because the sweep is keyed on what THIS job produced last time.
    MockFileSystem fs;
    ScriptedMediaEngine engine(fs);
    fs.AddDirectory("/out");
    AddExistingFile(fs, "/media/clip.mp4");

    ConversionJob job(MakeConversionOptions("/media/clip.mp4", "/out"), engine, fs);

    // Attempt 1 fails after the engine has already written its output.
    engine.failWith = ErrorInfo::Make("E_NET", ErrorCategory::NetworkError, "flaky");
    job.MarkStarting();
    job.MarkRunning();
    EXPECT_THROW(job.Execute(), MediaToolException);
    const std::string firstOutput = engine.lastOutputPath;
    EXPECT_NE(firstOutput.find("clip.mp3"), std::string::npos);
    // Simulate the partial output a killed encode would have left at that path.
    AddExistingFile(fs, firstOutput);
    AddExistingFile(fs, "/out/clip.processing.mp3");

    // Attempt 2 succeeds, and must land on the same name rather than "clip (1).mp3".
    engine.failWith.reset();
    job.MarkFailed(ErrorInfo::Make("E_NET", ErrorCategory::NetworkError, "flaky"));
    job.MarkRetrying();
    job.MarkRunning();
    job.Execute();

    EXPECT_EQ(engine.lastOutputPath, firstOutput)
        << "a retry must reclaim its own name, not sidestep to a new one";

    const auto& deleted = fs.DeletedPaths();
    const auto wasDeleted = [&deleted](const std::string& needle) {
        return std::any_of(deleted.begin(), deleted.end(), [&needle](const std::string& path) {
            return path.find(needle) != std::string::npos;
        });
    };
    EXPECT_TRUE(wasDeleted("clip.processing.mp3"));
    EXPECT_TRUE(wasDeleted("clip.mp3"));
}

TEST(MediaProcessingJob, AFirstAttemptNeverDeletesAFileItDidNotCreate) {
    // The reason the sweep is keyed on this job's own previous output rather than on
    // "whatever sits at the name we want": converting clip.mp4 into a folder that already
    // holds an unrelated clip.mp3 must not destroy the user's file.
    MockFileSystem fs;
    ScriptedMediaEngine engine(fs);
    fs.AddDirectory("/out");
    AddExistingFile(fs, "/media/clip.mp4");
    AddExistingFile(fs, "/out/clip.mp3");           // the user's, nothing to do with us
    AddExistingFile(fs, "/out/clip.processing.mp3"); // debris from some other tool

    ConversionJob job(MakeConversionOptions("/media/clip.mp4", "/out"), engine, fs);
    RunToCompletion(job);

    EXPECT_TRUE(fs.Exists("/out/clip.mp3")) << "a pre-existing user file was deleted";
    EXPECT_TRUE(fs.DeletedPaths().empty())
        << "a first attempt must not delete anything at all";
    EXPECT_NE(engine.lastOutputPath.find("clip (1).mp3"), std::string::npos)
        << "chose " << engine.lastOutputPath;
}

TEST(MediaProcessingJob, TheOutputDirectoryIsCreatedIfMissing) {
    MockFileSystem fs;
    ScriptedMediaEngine engine(fs);
    AddExistingFile(fs, "/media/clip.mp4");
    // /out deliberately not registered.

    ConversionJob job(MakeConversionOptions("/media/clip.mp4", "/out"), engine, fs);
    RunToCompletion(job);

    const auto& created = fs.CreatedDirectories();
    EXPECT_NE(std::find(created.begin(), created.end(), "/out"), created.end());
}
