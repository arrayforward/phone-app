#include "test_framework.hpp"

#include "tight/blocking_queue.hpp"

#include <chrono>
#include <cstdint>
#include <thread>

using namespace tight;
using namespace std::chrono_literals;

TEST_CASE(blocking_queue_fifo_order) {
    BlockingQueue<int> q;
    CHECK(q.push(1));
    CHECK(q.push(2));
    CHECK(q.push(3));
    CHECK_EQ(q.size(), 3U);
    CHECK_EQ(q.take(), std::optional<int>(1));
    CHECK_EQ(q.take(), std::optional<int>(2));
    CHECK_EQ(q.take(), std::optional<int>(3));
    CHECK_EQ(q.size(), 0U);
}

TEST_CASE(blocking_queue_poll) {
    BlockingQueue<int> q;
    CHECK(!q.poll().has_value());
    q.push(42);
    CHECK_EQ(q.poll(), std::optional<int>(42));
    CHECK(!q.poll().has_value());
}

TEST_CASE(blocking_queue_try_push_capacity) {
    BlockingQueue<int> q(2);
    CHECK(q.try_push(1));
    CHECK(q.try_push(2));
    CHECK(!q.try_push(3));
    CHECK_EQ(q.take(), std::optional<int>(1));
    CHECK(q.try_push(3));
    CHECK_EQ(q.take(), std::optional<int>(2));
    CHECK_EQ(q.take(), std::optional<int>(3));
}

TEST_CASE(blocking_queue_push_blocks_until_space) {
    BlockingQueue<int> q(1);
    q.push(1);
    bool pushed = false;
    std::thread t([&] {
        pushed = q.push(2);
    });
    std::this_thread::sleep_for(50ms);
    CHECK(!pushed);
    CHECK_EQ(q.take(), std::optional<int>(1));
    t.join();
    CHECK(pushed);
    CHECK_EQ(q.take(), std::optional<int>(2));
}

TEST_CASE(blocking_queue_take_for_timeout) {
    BlockingQueue<int> q;
    auto before = std::chrono::steady_clock::now();
    auto item = q.take_for(20ms);
    auto elapsed = std::chrono::steady_clock::now() - before;
    CHECK(!item.has_value());
    CHECK(elapsed < 2s);
}

TEST_CASE(blocking_queue_take_for_returns_item) {
    BlockingQueue<int> q;
    q.push(7);
    CHECK_EQ(q.take_for(50ms), std::optional<int>(7));
}

TEST_CASE(blocking_queue_close) {
    BlockingQueue<int> q;
    q.push(1);
    q.push(2);
    q.close();
    CHECK(q.is_closed());
    CHECK(!q.push(3));
    CHECK(!q.try_push(3));
    CHECK_EQ(q.take(), std::optional<int>(1));
    CHECK_EQ(q.take(), std::optional<int>(2));
    CHECK(!q.take().has_value());
    CHECK(!q.take_for(10ms).has_value());
}

TEST_CASE(blocking_queue_close_wakes_blocked_take) {
    BlockingQueue<int> q;
    std::optional<int> got;
    std::thread t([&] { got = q.take(); });
    std::this_thread::sleep_for(20ms);
    q.close();
    t.join();
    CHECK(!got.has_value());
}

TEST_CASE(blocking_queue_capacity_0_unbounded) {
    BlockingQueue<int> q(0);
    for (int i = 0; i < 10000; ++i) CHECK(q.try_push(i));
    CHECK_EQ(q.size(), 10000U);
    for (int i = 0; i < 10000; ++i) CHECK_EQ(q.take(), std::optional<int>(i));
}

TEST_CASE(blocking_queue_high_churn_node_reuse) {
    BlockingQueue<std::uint64_t> q;
    for (int round = 0; round < 1000; ++round) {
        for (int i = 0; i < 100; ++i) CHECK(q.push(static_cast<std::uint64_t>(i)));
        for (int i = 0; i < 100; ++i)
            CHECK_EQ(q.take(), std::optional<std::uint64_t>(static_cast<std::uint64_t>(i)));
    }
    CHECK_EQ(q.size(), 0U);
}
