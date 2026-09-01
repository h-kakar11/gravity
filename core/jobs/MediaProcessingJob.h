#pragma once

// The real Convert/Compress job type (Phase 2.6). One class handles both JobType::Conversion
// and JobType::Compression: per engines/ffmpeg/FFmpegArgBuilder.h, Compress is Convert
// with different default option VALUES supplied by the caller (main.cpp), not a
// structurally different pipeline -- so this job doesn't need two implementations either.
// Mirrors DownloadJob's shape closely: reserve an output filename (via the same
// FilenameReservationRegistry that fixes #12), call the engine with progress/cancel
// callbacks, verify the result, clean up (via the same CleanupJobArtifacts that fixes #3)
// on any failure.

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "core/filesystem/FilenameReservationRegistry.h"
#include "core/filesystem/IFileSystem.h"
#include "core/jobs/Job.h"
#include "core/media/IMediaEngine.h"

namespace mediatool::jobs {

class MediaProcessingJob final : public Job {
public:
    struct Options {
        std::string inputPath;
        std::string outputDirectory;
        std::string outputFormat;  // e.g. "mp4", "webp", "gif", "mp3" -- also the output extension
        // The full FFmpegArgBuilder options JSON (quality, videoCodec, hardwareAcceleration,
        // resolution, trim, watermark, audioBitrateKbps). `outputFormat` above is
        // authoritative for the job's own bookkeeping (output extension, Smart Rename);
        // engineOptions carries a copy of it too since IMediaEngine::Convert/Compress take
        // the whole shape as one opaque JSON blob.
        nlohmann::json engineOptions;
        bool isCompression = false;  // selects JobType::Compression vs Conversion, and which
                                      // IMediaEngine method to call

        // Called once, on the worker thread, the instant this job has reserved the output
        // filename it is about to write to -- the first moment anything knows what a
        // killed run would leave behind. The crash-recovery store records it so a later
        // launch can delete those artifacts before re-running the job; nothing else uses
        // it, and leaving it unset is a supported no-op.
        std::function<void(const std::string& outputDirectory, const std::string& filenameBase)>
            onArtifactLocation;
    };

    // `mediaEngine`, `fileSystem` and `reservationRegistry` must outlive this job.
    MediaProcessingJob(Options options, media::IMediaEngine& mediaEngine,
                       filesystem::IFileSystem& fileSystem,
                       filesystem::FilenameReservationRegistry& reservationRegistry);
    // `clock` must outlive this job. Lets tests inject a fixed/fake clock.
    MediaProcessingJob(Options options, media::IMediaEngine& mediaEngine,
                       filesystem::IFileSystem& fileSystem,
                       filesystem::FilenameReservationRegistry& reservationRegistry,
                       common::IClock& clock);

    void Execute() override;

private:
    Options options_;
    media::IMediaEngine& mediaEngine_;
    filesystem::IFileSystem& fileSystem_;
    filesystem::FilenameReservationRegistry& reservationRegistry_;
};

}  // namespace mediatool::jobs
