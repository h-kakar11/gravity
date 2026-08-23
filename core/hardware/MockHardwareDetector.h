#pragma once

// Fixed, test-supplied IHardwareDetector (spec section 37).

#include <utility>

#include "core/hardware/IHardwareDetector.h"

namespace mediatool::hardware {

class MockHardwareDetector : public IHardwareDetector {
public:
    explicit MockHardwareDetector(HardwareInfo info) : info_(std::move(info)) {}

    HardwareInfo Detect() override { return info_; }

private:
    HardwareInfo info_;
};

}  // namespace mediatool::hardware
