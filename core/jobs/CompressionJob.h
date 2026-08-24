#pragma once

// COMPRESSION: re-encode a local file smaller, keeping its container. Shares the whole
// lifecycle with ConversionJob via MediaProcessingJob -- see that header.

#include <string>

#include "core/jobs/MediaProcessingJob.h"
#include "core/media/ProcessingOptions.h"

namespace mediatool::jobs {

class CompressionJob final : public MediaProcessingJob {
public:
    struct Options {
        MediaProcessingJob::Options common;
        media::CompressionRequest request;
        // Container the compressed copy is written into. Compression keeps the source
        // container by default; this only exists because the output has to be named
        // something concrete before the encode starts.
        std::string outputExtension = "mp4";
    };

    CompressionJob(Options options, media::IMediaEngine& engine,
                   filesystem::IFileSystem& fileSystem);
    CompressionJob(Options options, media::IMediaEngine& engine,
                   filesystem::IFileSystem& fileSystem, common::IClock& clock);

protected:
    std::string TargetExtension() const override { return outputExtension_; }
    void Invoke(const std::string& inputPath, const std::string& outputPath) override;
    nlohmann::json DescribeMetadata() const override;
    std::string OperationLabel() const override { return "Compressing"; }

private:
    media::CompressionRequest request_;
    std::string outputExtension_;
};

}  // namespace mediatool::jobs
