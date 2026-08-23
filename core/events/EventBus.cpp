#include "core/events/EventBus.h"

#include <algorithm>

namespace mediatool::events {

SubscriptionId EventBus::Subscribe(Handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    const SubscriptionId id = nextId_++;
    subscribers_.push_back(Subscription{id, std::move(handler)});
    return id;
}

void EventBus::Unsubscribe(SubscriptionId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_.erase(
        std::remove_if(subscribers_.begin(), subscribers_.end(),
                        [id](const Subscription& sub) { return sub.id == id; }),
        subscribers_.end());
}

void EventBus::Publish(const Event& event) const {
    // Snapshot the subscriber list under the lock, then invoke handlers without holding
    // it -- a handler that calls Subscribe/Unsubscribe (or Publish, re-entrantly) would
    // otherwise deadlock on a non-recursive mutex.
    std::vector<Subscription> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = subscribers_;
    }
    for (const auto& sub : snapshot) {
        sub.handler(event);
    }
}

}  // namespace mediatool::events
