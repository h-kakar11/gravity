#include "core/common/IClock.h"

#include <chrono>
#include <cstdio>

namespace mediatool::common {

std::int64_t IClock::NowUnixMillis() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string SystemClock::NowIso8601Utc() const {
    using namespace std::chrono;

    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = system_clock::to_time_t(now);

    std::tm utcTm{};
#if defined(_WIN32)
    gmtime_s(&utcTm, &t);
#else
    gmtime_r(&t, &utcTm);
#endif

    // 64, not the 24 characters this actually formats to: each %d is an int, so GCC's
    // truncation analysis budgets 11 characters for every one of them and warns unless the
    // buffer covers that worst case. Sizing past the doubt is cheaper than a pragma and
    // keeps the build warning-free.
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  utcTm.tm_year + 1900, utcTm.tm_mon + 1, utcTm.tm_mday, utcTm.tm_hour,
                  utcTm.tm_min, utcTm.tm_sec, static_cast<int>(ms.count()));
    return std::string(buffer);
}

}  // namespace mediatool::common
