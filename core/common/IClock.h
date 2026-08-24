#pragma once

// One of the five mockable interfaces called out in spec section 37. Every timestamp
// (job createdAt/startedAt/completedAt, event timestamps) goes through this instead of
// calling the system clock directly, so tests can inject a fixed/fake clock.

#include <cstdint>
#include <string>

namespace mediatool::common {

class IClock {
public:
    virtual ~IClock() = default;

    // ISO-8601 UTC, e.g. "2026-08-23T14:03:11.512Z". This exact format is part of
    // docs/ipc-contract.md -- do not change it without updating that doc.
    virtual std::string NowIso8601Utc() const = 0;

    // Milliseconds since the Unix epoch. The queue scheduler needs to compare and subtract
    // instants (retry deadlines, fairness aging, persistence throttling), which is not
    // something you can do with a formatted string. Provided with a default implementation
    // derived from the system clock so that existing IClock implementations -- test fakes
    // included -- keep compiling; a fake that also wants deterministic millisecond control
    // overrides it.
    virtual std::int64_t NowUnixMillis() const;
};

class SystemClock final : public IClock {
public:
    std::string NowIso8601Utc() const override;
};

}  // namespace mediatool::common
