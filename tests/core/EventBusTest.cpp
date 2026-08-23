#include "core/events/EventBus.h"

#include <vector>

#include <gtest/gtest.h>

namespace mediatool::events {
namespace {

Event MakeTestEvent(std::string jobId) {
    return MakeEvent(EventType::JobStarted, nlohmann::json{{"state", "RUNNING"}}, std::move(jobId));
}

TEST(EventBusTest, PublishReachesAllCurrentSubscribersWithExactEvent) {
    EventBus bus;
    std::vector<Event> receivedByA;
    std::vector<Event> receivedByB;

    bus.Subscribe([&](const Event& e) { receivedByA.push_back(e); });
    bus.Subscribe([&](const Event& e) { receivedByB.push_back(e); });

    const Event event = MakeTestEvent("job-1");
    bus.Publish(event);

    ASSERT_EQ(receivedByA.size(), 1u);
    ASSERT_EQ(receivedByB.size(), 1u);
    EXPECT_EQ(receivedByA[0].type, event.type);
    EXPECT_EQ(receivedByA[0].jobId, event.jobId);
    EXPECT_EQ(receivedByA[0].data, event.data);
    EXPECT_EQ(receivedByA[0].timestampUtc, event.timestampUtc);
    EXPECT_EQ(receivedByB[0].data, event.data);
}

TEST(EventBusTest, UnsubscribeStopsDeliveryToThatSubscriberOnly) {
    EventBus bus;
    int countA = 0;
    int countB = 0;

    const SubscriptionId idA = bus.Subscribe([&](const Event&) { ++countA; });
    bus.Subscribe([&](const Event&) { ++countB; });

    bus.Publish(MakeTestEvent("job-1"));
    ASSERT_EQ(countA, 1);
    ASSERT_EQ(countB, 1);

    bus.Unsubscribe(idA);
    bus.Publish(MakeTestEvent("job-2"));

    EXPECT_EQ(countA, 1);  // unchanged -- unsubscribed
    EXPECT_EQ(countB, 2);  // still receiving
}

TEST(EventBusTest, PublishWithNoSubscribersDoesNotThrow) {
    EventBus bus;
    EXPECT_NO_THROW(bus.Publish(MakeTestEvent("job-1")));
}

TEST(EventBusTest, UnsubscribeUnknownIdIsHarmless) {
    EventBus bus;
    int count = 0;
    bus.Subscribe([&](const Event&) { ++count; });

    EXPECT_NO_THROW(bus.Unsubscribe(999999));

    bus.Publish(MakeTestEvent("job-1"));
    EXPECT_EQ(count, 1);
}

}  // namespace
}  // namespace mediatool::events
