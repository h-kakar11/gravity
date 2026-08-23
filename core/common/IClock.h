#pragma once

// One of the five mockable interfaces called out in spec section 37. Every timestamp
// (job createdAt/startedAt/completedAt, event timestamps) goes through this instead of
// calling the system clock directly, so tests can inject a fixed/fake clock.

#include <string>

namespace mediatool::common {

class IClock {
public:
    virtual ~IClock() = default;

    // ISO-8601 UTC, e.g. "2026-08-23T14:03:11.512Z". This exact format is part of
    // docs/ipc-contract.md -- do not change it without updating that doc.
    virtual std::string NowIso8601Utc() const = 0;
};

class SystemClock final : public IClock {
public:
    std::string NowIso8601Utc() const override;
};

}  // namespace mediatool::common
