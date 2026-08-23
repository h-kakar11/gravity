#pragma once

// Phase 1 implementation of downloads::IDownloadProvider (core/downloads/IDownloadProvider.h)
// that launches python/downloader/downloader.py over an IProcessRunner and translates its
// NDJSON stdout protocol (docs/ipc-contract.md) into the interface's callbacks. Everything
// outside this file talks to IDownloadProvider only -- do not treat yt-dlp as synonymous
// with the download architecture (spec section 19).

#include <string>

#include "core/downloads/IDownloadProvider.h"
#include "core/process/IProcessRunner.h"

namespace mediatool::downloader {

class YtDlpProvider : public downloads::IDownloadProvider {
public:
    // `pythonExecutable` and `scriptPath` are injected rather than hardcoded so tests and
    // deployment can point at whatever venv/interpreter and script location apply.
    YtDlpProvider(process::IProcessRunner& processRunner, std::string pythonExecutable,
                  std::string scriptPath);

    // True for anything that looks like an http/https URL -- deliberately not
    // youtube.com-only, so this extends to other yt-dlp-supported sites later (spec
    // section 20). yt-dlp itself rejects what it can't actually handle.
    bool CanHandle(const std::string& url) const override;

    void Download(const downloads::DownloadOptions& options, downloads::MetadataCallback onMetadata,
                  downloads::ProgressCallback onProgress, downloads::CompletedCallback onCompleted,
                  downloads::CancelledCallback isCancelled) override;

private:
    process::IProcessRunner& processRunner_;
    std::string pythonExecutable_;
    std::string scriptPath_;
};

}  // namespace mediatool::downloader
