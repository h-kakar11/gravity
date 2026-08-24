#pragma once

// CONVERSION: re-container/re-encode a local file into a different format. All of the
// lifecycle (validation, output naming, verification, cleanup) lives in MediaProcessingJob;
// this class only knows which engine call to make and what to advertise about itself.

#include <string>

#include "core/jobs/MediaProcessingJob.h"
#include "core/media/ProcessingOptions.h"

namespace mediatool::jobs {

class ConversionJob final : public MediaProcessingJob {
public:
    struct Options {
        MediaProcessingJob::Options common;
        media::ConversionRequest request;
    };

    ConversionJob(Options options, media::IMediaEngine& engine,
                  filesystem::IFileSystem& fileSystem);
    ConversionJob(Options options, media::IMediaEngine& engine,
                  filesystem::IFileSystem& fileSystem, common::IClock& clock);

protected:
    std::string TargetExtension() const override;
    void Invoke(const std::string& inputPath, const std::string& outputPath) override;
    nlohmann::json DescribeMetadata() const override;
    std::string OperationLabel() const override { return "Converting"; }

private:
    media::ConversionRequest request_;
};

}  // namespace mediatool::jobs
