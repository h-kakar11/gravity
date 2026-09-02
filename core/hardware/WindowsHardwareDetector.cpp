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

// Non-Windows fallback. Gravity ships on Windows only, but CMakeLists.txt documents a
// Linux/WSL build as the supported way to run the ASan/UBSan preset (MinGW has no
// reliable ASan runtime and no TSan at all), and the stress/concurrency suites this is
// meant to exercise are not Windows-specific. Without a definition here the whole
// non-Windows build fails to link on WindowsHardwareDetector's vtable, so that
// documented path could not actually be taken.
//
// This reports only what is portable: the logical core count. An empty GPU and encoder
// list is an explicitly valid result (see HardwareInfo.h, spec section 22), so this is a
// truthful answer rather than a stub that fakes hardware.

#include <thread>

namespace mediatool::hardware {

HardwareInfo WindowsHardwareDetector::Detect() {
    HardwareInfo info;
    try {
        info.cpu.logicalCores = std::thread::hardware_concurrency();
        info.cpu.name = "Unknown CPU";
        info.gpus = {};
        info.availableEncoders = {};
    } catch (...) {
        // Detect() must never throw -- same contract as the Windows implementation.
    }
    return info;
}

}  // namespace mediatool::hardware

#endif  // _WIN32
