#pragma once

// One of the five mockable interfaces called out in spec section 37.

#include "core/hardware/HardwareInfo.h"

namespace mediatool::hardware {

class IHardwareDetector {
public:
    virtual ~IHardwareDetector() = default;
    virtual HardwareInfo Detect() = 0;
};

}  // namespace mediatool::hardware
