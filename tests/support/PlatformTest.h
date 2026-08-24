#pragma once

// Windows is the product's target platform, but the C++ core is also built and tested on
// Linux dev/CI machines (see docs/development.md). A handful of tests assert genuinely
// Windows-specific behaviour -- backslash path separators, drive-letter roots, `cmd.exe` --
// and are meaningless rather than failing off-Windows.
//
// Use SKIP_UNLESS_WINDOWS() as the first line of such a test so a non-Windows run reports
// it as SKIPPED instead of FAILED. Do NOT reach for this to silence a test that fails for
// a real, platform-independent reason.

#include <gtest/gtest.h>

#ifdef _WIN32
#define SKIP_UNLESS_WINDOWS() ((void)0)
#else
#define SKIP_UNLESS_WINDOWS() \
    GTEST_SKIP() << "Windows-specific behaviour; not applicable on this platform"
#endif
