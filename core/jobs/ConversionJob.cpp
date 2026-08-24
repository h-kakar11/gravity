#include "core/jobs/ConversionJob.h"

#include <utility>

namespace mediatool::jobs {

ConversionJob::ConversionJob(Options options, media::IMediaEngine& engine,
                             filesystem::IFileSystem& fileSystem)
    : MediaProcessingJob(JobType::Conversion, std::move(options.common), engine, fileSystem),
      request_(options.request) {}

ConversionJob::ConversionJob(Options options, media::IMediaEngine& engine,
                             filesystem::IFileSystem& fileSystem, common::IClock& clock)
    : MediaProcessingJob(JobType::Conversion, std::move(options.common), engine, fileSystem, clock),
      request_(options.request) {}

std::string ConversionJob::TargetExtension() const {
    return media::ExtensionFor(request_.targetFormat);
}

void ConversionJob::Invoke(const std::string& inputPath, const std::string& outputPath) {
    engine().Convert(inputPath, outputPath, request_.ToJson(), EngineProgressSink(),
                     CancellationProbe());
}

nlohmann::json ConversionJob::DescribeMetadata() const {
    nlohmann::json metadata;
    metadata["operation"] = "CONVERSION";
    metadata["targetFormatName"] = media::ToWireString(request_.targetFormat);
    metadata["audioOnly"] = media::IsAudioOnly(request_.targetFormat);
    if (request_.audioBitrateKbps) metadata["audioBitrateKbps"] = *request_.audioBitrateKbps;
    if (request_.maxHeight) metadata["maxHeight"] = *request_.maxHeight;
    if (request_.gifFps) metadata["gifFps"] = *request_.gifFps;
    return metadata;
}

}  // namespace mediatool::jobs
