#pragma once

// Interface only for Phase 1 (spec section 2, spec section 34: do not over-engineer image
// processing yet). No .cpp / implementation exists -- a later phase provides one (e.g.
// backed by libvips or WIC) and wires it into engines/CMakeLists.txt. Mirrors the shape
// of core/media/IMediaEngine.h so job/engine wiring stays consistent across media types.

#include <string>

#include <nlohmann/json.hpp>

#include "core/filesystem/FileInfo.h"

namespace mediatool::images {

class IImageEngine {
public:
    virtual ~IImageEngine() = default;

    // False if the underlying image backend could not be discovered/initialized on this
    // machine -- callers must tolerate image processing being unavailable, same as
    // media::IMediaEngine::IsAvailable().
    virtual bool IsAvailable() const = 0;

    // Throws errors::MediaToolException on failure (missing file, unreadable format).
    virtual filesystem::FileInfo Probe(const std::string& path) = 0;

    virtual void Convert(const std::string& inputPath, const std::string& outputPath,
                        const nlohmann::json& options) = 0;

    virtual void Compress(const std::string& inputPath, const std::string& outputPath,
                         const nlohmann::json& options) = 0;

    virtual void Resize(const std::string& inputPath, const std::string& outputPath,
                       int width, int height, const nlohmann::json& options) = 0;
};

}  // namespace mediatool::images
