#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "core/hardware/HardwareInfo.h"
#include "core/hardware/MockHardwareDetector.h"
#include "core/hardware/WindowsHardwareDetector.h"

using namespace mediatool::hardware;

TEST(HardwareInfoTest, GpuVendorWireStrings) {
    EXPECT_EQ(ToWireString(GpuVendor::Nvidia), "NVIDIA");
    EXPECT_EQ(ToWireString(GpuVendor::Amd), "AMD");
    EXPECT_EQ(ToWireString(GpuVendor::Intel), "INTEL");
    EXPECT_EQ(ToWireString(GpuVendor::Unknown), "UNKNOWN");
}

TEST(HardwareInfoTest, ToJsonProducesContractShape) {
    HardwareInfo info;
    info.cpu = {"Test CPU", 8};
    info.gpus = {GpuInfo{GpuVendor::Nvidia, "Test GPU"}};
    info.availableEncoders = {"H264_NVENC"};

    nlohmann::json json = info.ToJson();

    EXPECT_EQ(json["cpu"]["name"], "Test CPU");
    EXPECT_EQ(json["cpu"]["logicalCores"], 8);
    ASSERT_EQ(json["gpus"].size(), 1u);
    EXPECT_EQ(json["gpus"][0]["vendor"], "NVIDIA");
    EXPECT_EQ(json["gpus"][0]["name"], "Test GPU");
    ASSERT_EQ(json["availableEncoders"].size(), 1u);
    EXPECT_EQ(json["availableEncoders"][0], "H264_NVENC");
}

TEST(MockHardwareDetectorTest, ReturnsExactlyWhatWasConfigured) {
    HardwareInfo configured;
    configured.cpu = {"Mock CPU", 4};
    configured.gpus = {GpuInfo{GpuVendor::Amd, "Mock GPU"}};
    configured.availableEncoders = {"H264_AMF"};

    MockHardwareDetector detector(configured);
    HardwareInfo result = detector.Detect();

    EXPECT_EQ(result.cpu.name, "Mock CPU");
    EXPECT_EQ(result.cpu.logicalCores, 4u);
    ASSERT_EQ(result.gpus.size(), 1u);
    EXPECT_EQ(result.gpus[0].vendor, GpuVendor::Amd);
    EXPECT_EQ(result.gpus[0].name, "Mock GPU");
    EXPECT_EQ(result.availableEncoders, (std::vector<std::string>{"H264_AMF"}));
}

TEST(WindowsHardwareDetectorTest, DetectDoesNotThrowAndReportsLogicalCores) {
    WindowsHardwareDetector detector;
    HardwareInfo info;

    EXPECT_NO_THROW(info = detector.Detect());

    // Deliberately no assertion on GPU vendor/count -- the test machine's hardware is
    // unknown and an empty GPU list is a correct result.
    EXPECT_GT(info.cpu.logicalCores, 0u);
}
