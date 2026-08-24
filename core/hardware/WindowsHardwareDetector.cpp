#include "core/hardware/WindowsHardwareDetector.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <dxgi.h>

#include <thread>
#include <utility>

namespace mediatool::hardware {

namespace {

// IDXGIFactory1's well-known IID, defined locally so this translation unit doesn't need
// to link against dxguid (only "dxgi" is linked -- see core/CMakeLists.txt) and doesn't
// rely on the MSVC-only __uuidof() extension.
constexpr GUID kIidIDXGIFactory1 = {
    0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};

std::string ReadCpuNameFromRegistry() {
    HKEY key = nullptr;
    LONG openResult = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                                     R"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)", 0,
                                     KEY_READ, &key);
    if (openResult != ERROR_SUCCESS) {
        return "Unknown CPU";
    }

    char buffer[256] = {};
    DWORD bufferSize = sizeof(buffer) - 1;  // leave room for a guaranteed null terminator
    DWORD valueType = 0;
    LONG queryResult = RegQueryValueExA(key, "ProcessorNameString", nullptr, &valueType,
                                         reinterpret_cast<LPBYTE>(buffer), &bufferSize);
    RegCloseKey(key);

    if (queryResult != ERROR_SUCCESS || valueType != REG_SZ) {
        return "Unknown CPU";
    }

    std::string name(buffer);
    while (!name.empty() && name.back() == ' ') {
        name.pop_back();  // registry values are commonly padded with trailing spaces
    }
    return name.empty() ? "Unknown CPU" : name;
}

std::string WideToUtf8(const wchar_t* wide) {
    if (wide == nullptr) {
        return {};
    }
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 1) {
        return {};
    }
    std::string result(static_cast<size_t>(sizeNeeded - 1), '\0');  // exclude null terminator
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), sizeNeeded, nullptr, nullptr);
    return result;
}

GpuVendor ClassifyVendor(UINT vendorId) {
    switch (vendorId) {
        case 0x10DE:
            return GpuVendor::Nvidia;
        case 0x1002:
        case 0x1022:
            return GpuVendor::Amd;
        case 0x8086:
            return GpuVendor::Intel;
        default:
            return GpuVendor::Unknown;
    }
}

std::vector<GpuInfo> EnumerateGpus() {
    std::vector<GpuInfo> gpus;

    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(kIidIDXGIFactory1, reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || factory == nullptr) {
        return gpus;
    }

    UINT index = 0;
    IDXGIAdapter1* adapter = nullptr;
    while (factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            std::string name = WideToUtf8(desc.Description);
            // Skip the software "Basic Render Driver" adapter -- it is not a real GPU.
            constexpr UINT kMicrosoftVendorId = 0x1414;
            bool isBasicRenderDriver =
                desc.VendorId == kMicrosoftVendorId || name.find("Microsoft Basic Render") != std::string::npos;
            if (!isBasicRenderDriver) {
                GpuInfo info;
                info.name = name;
                info.vendor = ClassifyVendor(desc.VendorId);
                gpus.push_back(std::move(info));
            }
        }
        adapter->Release();
        adapter = nullptr;
        ++index;
    }

    factory->Release();
    return gpus;
}

}  // namespace

HardwareInfo WindowsHardwareDetector::Detect() {
    HardwareInfo info;
    try {
        info.cpu.logicalCores = std::thread::hardware_concurrency();
        info.cpu.name = ReadCpuNameFromRegistry();
        info.gpus = EnumerateGpus();
        // Real NVENC/QSV/AMF capability probing (querying vendor SDKs or shelling out to
        // `ffmpeg -encoders`) is a documented Phase 2 TODO (spec section 22) -- do not
        // hardcode NVIDIA or any other vendor's encoder availability here.
        info.availableEncoders = {};
    } catch (...) {
        // Detect() must never throw -- an empty/partial result is a valid outcome on an
        // unusual machine.
    }
    return info;
}

}  // namespace mediatool::hardware

#else  // !_WIN32

// Non-Windows fallback. Windows is the product's target platform, but the C++ core is
// built and its test suite is run on Linux CI/dev machines too, and main.cpp instantiates
// this detector unconditionally -- without a definition here the whole executable fails to
// link off-Windows. Reads what POSIX can offer cheaply and honestly reports the rest as
// unknown rather than inventing plausible-looking hardware. See docs/decisions.md.

#include <fstream>
#include <thread>

namespace mediatool::hardware {

namespace {

std::string ReadCpuNameFromProcCpuinfo() {
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (!cpuinfo) return "Unknown CPU";
    std::string line;
    while (std::getline(cpuinfo, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = line.substr(0, line.find_last_not_of(" \t", colon - 1) + 1);
        if (key != "model name" && key != "Model" && key != "Processor") continue;
        const auto valueStart = line.find_first_not_of(" \t", colon + 1);
        if (valueStart == std::string::npos) continue;
        return line.substr(valueStart);
    }
    return "Unknown CPU";
}

}  // namespace

HardwareInfo WindowsHardwareDetector::Detect() {
    HardwareInfo info;
    try {
        info.cpu.logicalCores = std::thread::hardware_concurrency();
        info.cpu.name = ReadCpuNameFromProcCpuinfo();
        // No DXGI off Windows. Enumerating GPUs would mean a second, unrelated
        // vendor-detection implementation for a platform the product does not ship on --
        // report none rather than guess.
        info.gpus = {};
        info.availableEncoders = {};
    } catch (...) {
        // Detect() must never throw -- an empty/partial result is a valid outcome.
    }
    return info;
}

}  // namespace mediatool::hardware

#endif  // _WIN32
