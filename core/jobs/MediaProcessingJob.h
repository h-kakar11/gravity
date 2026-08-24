#pragma once

// Shared lifecycle for the two local-file media operations, CONVERSION and COMPRESSION.
// Both do the same thing around the edges -- validate the input still exists, derive a
// collision-free output path, sweep leftover artifacts from a previous attempt, run an
// IMediaEngine call with progress/cancellation plumbed through, verify what came out, and
// publish a result -- and differ only in which engine call runs and what metadata they
// advertise. That difference is the two virtuals at the bottom; everything else lives here
// exactly once.
//
// Talks to IMediaEngine and IFileSystem only, never to FFmpegEngine or std::filesystem
// directly, so both subclasses are fully testable against the Mock* implementations.

#include <string>

#include <nlohmann/json.hpp>

#include "core/filesystem/IFileSystem.h"
#include "core/jobs/Job.h"
#include "core/media/IMediaEngine.h"

namespace mediatool::jobs {

class MediaProcessingJob : public Job {
public:
    struct Options {
        std::string inputPath;
        std::string outputDirectory;
        // Leave empty to derive "<input stem>.<target extension>" from the input, made
        // collision-free against `outputDirectory`. When set, it is used as the base name
        // (with any extension replaced by the target one) and still collision-checked.
        std::string outputFilenameBase;
    };

    // Final: the surrounding lifecycle is fixed. Subclasses customize it through
    // TargetExtension()/Invoke()/DescribeMetadata(), not by re-running the sequence.
    void Execute() final;

protected:
    MediaProcessingJob(JobType type, Options options, media::IMediaEngine& engine,
                       filesystem::IFileSystem& fileSystem);
    MediaProcessingJob(JobType type, Options options, media::IMediaEngine& engine,
                       filesystem::IFileSystem& fileSystem, common::IClock& clock);

    // Extension (no leading dot) the finished output will carry.
    virtual std::string TargetExtension() const = 0;

    // Runs the actual engine operation. Implementations call engine().Convert/Compress
    // with the callbacks ReportEngineProgress()/CancellationProbe() hand them.
    virtual void Invoke(const std::string& inputPath, const std::string& outputPath) = 0;

    // Operation-specific fields merged into the job's metadata alongside the shared
    // input/output ones (spec section 21). Must never include a raw ffmpeg command line.
    virtual nlohmann::json DescribeMetadata() const = 0;

    // Short human-readable verb for status messages, e.g. "Converting".
    virtual std::string OperationLabel() const = 0;

    media::IMediaEngine& engine() { return engine_; }
    const Options& options() const { return options_; }

    media::ProgressCallback EngineProgressSink();
    media::CancelledCallback CancellationProbe();

private:
    // Removes leftovers a previous attempt of THIS job may have left next to the output:
    // AtomicWriter's "<name>.processing.<ext>" temporary, and a fully written output from
    // an attempt that was later rejected. Only touches names this job itself chose, and
    // only ever runs before the engine writes anything, so it cannot delete a user file
    // that merely happens to sit in the same folder.
    void SweepPreviousAttempt(const std::string& outputPath);

    Options options_;
    media::IMediaEngine& engine_;
    filesystem::IFileSystem& fileSystem_;
};

}  // namespace mediatool::jobs
