#pragma once

// Simple in-process pub/sub bus (spec section 8). This is what JobManager's state-change
// callbacks and the logger's event sink get wired into by a later integration pass -- it
// has no knowledge of jobs, IPC, or logging itself.

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "core/events/Event.h"

namespace mediatool::events {

using SubscriptionId = std::uint64_t;

class EventBus {
public:
    using Handler = std::function<void(const Event&)>;

    // Returns an opaque id; pass it to Unsubscribe to stop delivery.
    SubscriptionId Subscribe(Handler handler);
    void Unsubscribe(SubscriptionId id);

    // Invokes every subscriber current at call time, synchronously, on the calling
    // thread, in subscription order. Phase 1 deliberately has no async dispatch queue --
    // a handler that needs to hop threads (e.g. to write to stdout) must do so itself.
    void Publish(const Event& event) const;

private:
    struct Subscription {
        SubscriptionId id;
        Handler handler;
    };

    mutable std::mutex mutex_;
    std::vector<Subscription> subscribers_;
    SubscriptionId nextId_ = 1;
};

}  // namespace mediatool::events
