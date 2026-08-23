#pragma once

// Hardware detection result (spec section 22). Phase 1 detects CPU core count/name and
// enumerates GPU adapters with a best-effort vendor classification; encoder capability
// probing (which NVENC/QSV/AMF profiles are actually usable) is a documented TODO left
// for a phase that can afford to shell out to ffmpeg -encoders or query vendor SDKs.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mediatool::hardware {

enum class GpuVendor {
    Nvidia,
    Amd,
    Intel,
    Unknown,
};

std::string ToWireString(GpuVendor vendor);

struct CpuInfo {
    std::string name;
    unsigned int logicalCores = 0;
};

struct GpuInfo {
    GpuVendor vendor = GpuVendor::Unknown;
    std::string name;
};

struct HardwareInfo {
    CpuInfo cpu;
    std::vector<GpuInfo> gpus;
    // Best-effort, may be empty. Never assume non-empty -- the app must work correctly
    // with zero hardware encoders (spec section 22: do not hard-code NVIDIA presence).
    std::vector<std::string> availableEncoders;

    nlohmann::json ToJson() const;
};

}  // namespace mediatool::hardware
