#pragma once

// Real IHardwareDetector for Windows (spec sections 22, 37). Detect() must never throw --
// any failure to read a piece of hardware info falls back to a safe default rather than
// propagating.

#include "core/hardware/IHardwareDetector.h"

namespace mediatool::hardware {

class WindowsHardwareDetector : public IHardwareDetector {
public:
    HardwareInfo Detect() override;
};

}  // namespace mediatool::hardware
