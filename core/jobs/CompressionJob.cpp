#include "core/jobs/CompressionJob.h"

#include <utility>

namespace mediatool::jobs {

CompressionJob::CompressionJob(Options options, media::IMediaEngine& engine,
                               filesystem::IFileSystem& fileSystem)
    : MediaProcessingJob(JobType::Compression, std::move(options.common), engine, fileSystem),
      request_(options.request),
      outputExtension_(options.outputExtension.empty() ? "mp4" : options.outputExtension) {}

CompressionJob::CompressionJob(Options options, media::IMediaEngine& engine,
                               filesystem::IFileSystem& fileSystem, common::IClock& clock)
    : MediaProcessingJob(JobType::Compression, std::move(options.common), engine, fileSystem, clock),
      request_(options.request),
      outputExtension_(options.outputExtension.empty() ? "mp4" : options.outputExtension) {}

void CompressionJob::Invoke(const std::string& inputPath, const std::string& outputPath) {
    engine().Compress(inputPath, outputPath, request_.ToJson(), EngineProgressSink(),
                      CancellationProbe());
}

nlohmann::json CompressionJob::DescribeMetadata() const {
    nlohmann::json metadata;
    metadata["operation"] = "COMPRESSION";
    metadata["preset"] = media::ToWireString(request_.preset);
    if (request_.maxHeight) metadata["maxHeight"] = *request_.maxHeight;
    if (request_.audioBitrateKbps) metadata["audioBitrateKbps"] = *request_.audioBitrateKbps;
    return metadata;
}

}  // namespace mediatool::jobs
