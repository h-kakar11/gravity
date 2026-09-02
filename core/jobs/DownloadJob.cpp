#include "core/jobs/DownloadJob.h"

#include <utility>

#include "core/errors/MediaToolException.h"
#include "core/filesystem/FilenameSanitizer.h"
#include "core/filesystem/PathUtils.h"
#include "core/jobs/JobArtifactCleanup.h"

namespace mediatool::jobs {

namespace {

[[noreturn]] void ThrowCancelled() {
    throw errors::MediaToolException(errors::ErrorInfo::Make(
        "E_DOWNLOAD_JOB_CANCELLED", errors::ErrorCategory::Cancelled, "Download job was cancelled."));
}

nlohmann::json MetadataToJson(const downloads::DownloadMetadata& metadata) {
    nlohmann::json json;
    json["title"] = metadata.title;
    if (metadata.uploader) json["uploader"] = *metadata.uploader;
    if (metadata.durationSeconds) json["durationSeconds"] = *metadata.durationSeconds;
    if (metadata.webpageUrl) json["webpageUrl"] = *metadata.webpageUrl;
    if (metadata.thumbnailUrl) json["thumbnailUrl"] = *metadata.thumbnailUrl;
    return json;
}

}  // namespace

DownloadJob::DownloadJob(Options options, downloads::IDownloadProvider& provider,
                          filesystem::IFileSystem& fileSystem, media::IMediaEngine* mediaEngine,
                          filesystem::FilenameReservationRegistry& reservationRegistry)
    : Job(JobType::Download),
      options_(std::move(options)),
      provider_(provider),
      fileSystem_(fileSystem),
      mediaEngine_(mediaEngine),
      reservationRegistry_(reservationRegistry) {}

DownloadJob::DownloadJob(Options options, downloads::IDownloadProvider& provider,
                          filesystem::IFileSystem& fileSystem, media::IMediaEngine* mediaEngine,
                          filesystem::FilenameReservationRegistry& reservationRegistry,
                          common::IClock& clock)
    : Job(JobType::Download, clock),
      options_(std::move(options)),
      provider_(provider),
      fileSystem_(fileSystem),
      mediaEngine_(mediaEngine),
      reservationRegistry_(reservationRegistry) {}

void DownloadJob::CleanupArtifacts(const std::string& filenameBase) {
    CleanupJobArtifacts(fileSystem_, options_.outputDirectory, filenameBase);
}

void DownloadJob::Execute() {
    Progress fetching;
    fetching.statusMessage = "Fetching metadata";
    ReportProgress(fetching);

    if (IsCancellationRequested()) ThrowCancelled();

    const downloads::DownloadMetadata metadata =
        provider_.Inspect(options_.url, [this] { return IsCancellationRequested(); });
    SetMetadata(MetadataToJson(metadata));

    std::string safeTitle = filesystem::SanitizeWindowsFilename(metadata.title);
    // Numbering goes on BEFORE the MAX_PATH truncation, not after: the prefix is part of
    // the name that has to fit, and adding it afterwards could push an
    // already-at-the-limit name back over it.
    if (options_.playlistIndex && options_.playlistCount) {
        safeTitle =
            filesystem::WithPlaylistIndex(safeTitle, *options_.playlistIndex, *options_.playlistCount);
    }
    safeTitle = filesystem::TruncateBaseNameForMaxPath(options_.outputDirectory, safeTitle);
    fileSystem_.CreateDirectory(options_.outputDirectory);
    // Reserve (not just probe) the output base name: DeduplicateBaseName alone only
    // checks the disk, which is a TOCTOU race once concurrency > 1 -- two jobs racing to
    // download videos with the same title could both compute the same "next free" name
    // (#12). The reservation is released automatically when it goes out of scope at the
    // end of this function (success or exception alike), freeing the name for reuse once
    // this job is no longer using it.
    auto reservation =
        reservationRegistry_.Reserve(options_.outputDirectory, safeTitle, fileSystem_);
    const std::string& filenameBase = reservation.BaseName();
    if (options_.onArtifactLocation) {
        options_.onArtifactLocation(options_.outputDirectory, filenameBase);
    }

    Progress starting;
    starting.statusMessage = "Starting download";
    ReportProgress(starting);

    downloads::DownloadOptions downloadOptions;
    downloadOptions.url = options_.url;
    downloadOptions.outputDirectory = options_.outputDirectory;
    downloadOptions.quality = options_.quality;
    downloadOptions.formatId = options_.formatId;
    downloadOptions.filenameBase = filenameBase;

    std::string outputPath;
    try {
        provider_.Download(
            downloadOptions, [](const downloads::DownloadMetadata&) { /* already have it */ },
            [this](const Progress& progress) { ReportProgress(progress); },
            [&outputPath](const std::string& path) { outputPath = path; },
            [this] { return IsCancellationRequested(); });
    } catch (...) {
        // catch(...), not catch(MediaToolException&): the point of this handler is that
        // NOTHING leaves a half-written .part file behind. A provider that lets anything
        // else escape used to skip cleanup entirely, which is the one case where the
        // artifact outlives every code path that knows its name.
        CleanupArtifacts(filenameBase);
        throw;
    }

    if (outputPath.empty() || !fileSystem_.Exists(outputPath)) {
        CleanupArtifacts(filenameBase);
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_DOWNLOAD_OUTPUT_MISSING", errors::ErrorCategory::DownloadFailure,
            "The downloader reported success but the output file is missing.",
            "expected output at: " + outputPath));
    }

    filesystem::FileInfo info = fileSystem_.Inspect(outputPath);
    if (info.sizeBytes == 0) {
        CleanupArtifacts(filenameBase);
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_DOWNLOAD_OUTPUT_EMPTY", errors::ErrorCategory::DownloadFailure,
            "The downloaded file is empty.", outputPath));
    }

    if (mediaEngine_ && mediaEngine_->IsAvailable()) {
        try {
            const filesystem::FileInfo probed = mediaEngine_->Probe(outputPath);
            info.durationSeconds = probed.durationSeconds;
            info.width = probed.width;
            info.height = probed.height;
            info.videoCodec = probed.videoCodec;
            info.audioCodec = probed.audioCodec;
            info.bitrate = probed.bitrate;
            info.fps = probed.fps;
        } catch (const errors::MediaToolException& e) {
            CleanupArtifacts(filenameBase);
            throw errors::MediaToolException(errors::ErrorInfo::Make(
                "E_DOWNLOAD_VERIFICATION_FAILED", errors::ErrorCategory::InvalidFile,
                "The downloaded file failed media verification.", e.Info().details));
        }
    }

    nlohmann::json result;
    result["outputPath"] = outputPath;
    result["fileInfo"] = info.ToJson();
    SetResult(result);
}

}  // namespace mediatool::jobs
