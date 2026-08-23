#include "core/hardware/HardwareInfo.h"

namespace mediatool::hardware {

std::string ToWireString(GpuVendor vendor) {
    switch (vendor) {
        case GpuVendor::Nvidia:
            return "NVIDIA";
        case GpuVendor::Amd:
            return "AMD";
        case GpuVendor::Intel:
            return "INTEL";
        case GpuVendor::Unknown:
            return "UNKNOWN";
    }
    return "UNKNOWN";
}

nlohmann::json HardwareInfo::ToJson() const {
    nlohmann::json gpuArray = nlohmann::json::array();
    for (const auto& gpu : gpus) {
        gpuArray.push_back({
            {"vendor", ToWireString(gpu.vendor)},
            {"name", gpu.name},
        });
    }

    return nlohmann::json{
        {"cpu", {{"name", cpu.name}, {"logicalCores", cpu.logicalCores}}},
        {"gpus", gpuArray},
        {"availableEncoders", availableEncoders},
    };
}

}  // namespace mediatool::hardware
