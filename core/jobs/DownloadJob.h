#pragma once

// The Phase 2 real download job (spec sections 3, 14). Owns exactly the DOWNLOAD job
// type's lifecycle: fetch metadata, pick a collision-free output filename, drive the
// download through IDownloadProvider, then verify the result before reporting success --
// never trusting "the downloader exited 0" alone (spec section 27). Talks to
// IDownloadProvider and IFileSystem only, never to YtDlpProvider/Python or
// std::filesystem directly, so it's fully testable against the Mock* implementations.

#include <string>

#include "core/downloads/IDownloadProvider.h"
#include "core/downloads/QualityPreset.h"
#include "core/filesystem/IFileSystem.h"
#include "core/jobs/Job.h"
#include "core/media/IMediaEngine.h"

namespace mediatool::jobs {

class DownloadJob final : public Job {
public:
    struct Options {
        std::string url;
        std::string outputDirectory;
        downloads::QualityPreset quality = downloads::QualityPreset::Best;
    };

    // `provider` and `fileSystem` must outlive this job. `mediaEngine` may be nullptr --
    // output verification then relies on existence + non-zero size only, skipping the
    // ffprobe cross-check (see Execute()).
    DownloadJob(Options options, downloads::IDownloadProvider& provider,
                filesystem::IFileSystem& fileSystem, media::IMediaEngine* mediaEngine);
    // `clock` must outlive this job. Lets tests inject a fixed/fake clock, mirroring
    // TestJob's (JobType, IClock&) constructor.
    DownloadJob(Options options, downloads::IDownloadProvider& provider,
                filesystem::IFileSystem& fileSystem, media::IMediaEngine* mediaEngine,
                common::IClock& clock);

    void Execute() override;

private:
    // Best-effort: deletes every file in outputDirectory that filesystem::IsJobArtifactOf
    // recognizes as belonging to `filenameBase` (yt-dlp's own ".part"/intermediate-format
    // artifacts, sidecar metadata, or a fully written but since-rejected output) -- never
    // a bare prefix match, and never a recursive directory delete (uses
    // IFileSystem::DeleteFile, not Delete). Safe because DeduplicateBaseName chose
    // `filenameBase` specifically to not collide with anything that predates this job, so
    // anything IsJobArtifactOf accepts was created by this run. Never lets a cleanup
    // failure mask the real job error.
    void CleanupArtifacts(const std::string& filenameBase);

    Options options_;
    downloads::IDownloadProvider& provider_;
    filesystem::IFileSystem& fileSystem_;
    media::IMediaEngine* mediaEngine_;
};

}  // namespace mediatool::jobs
