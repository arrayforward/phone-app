#include "test_framework.hpp"

#include "util/small_thread.hpp"

#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

using tight::tight_detail::SmallThread;

// 基本执行：线程内设置标志/累加，join 后可见
TEST_CASE(test_small_thread_basic_run) {
    std::atomic<bool> ran{false};
    std::atomic<int> sum{0};
    SmallThread t([&] {
        for (int i = 1; i <= 100; ++i) sum.fetch_add(i);
        ran = true;
    });
    CHECK(t.joinable());
    t.join();
    CHECK(!t.joinable());
    CHECK(ran);
    CHECK_EQ(sum.load(), 5050);
}

// stack_bytes=0（系统默认）与 64KB 两种栈尺寸都能正常工作
TEST_CASE(test_small_thread_stack_sizes) {
    for (std::size_t stack_bytes : {std::size_t(0), std::size_t(64 * 1024)}) {
        std::atomic<bool> ran{false};
        SmallThread t([&] { ran = true; }, stack_bytes);
        CHECK(t.joinable());
        t.join();
        CHECK(ran);
    }
}

// 不 join 直接析构：自动 join，任务完整执行，不 terminate
TEST_CASE(test_small_thread_destructor_auto_join) {
    std::atomic<bool> done{false};
    {
        SmallThread t([&] {
            for (volatile int i = 0; i < 100000; ++i) {}
            done = true;
        });
        // 不调用 join，离开作用域由析构自动 join
    }
    CHECK(done); // 析构已等待线程完成
}

// 移动构造/移动赋值：原对象不可 join、新对象可 join 且任务正常完成
TEST_CASE(test_small_thread_move_semantics) {
    std::atomic<int> counter{0};

    // 移动构造
    SmallThread a([&] { counter.fetch_add(1); });
    CHECK(a.joinable());
    SmallThread b(std::move(a));
    CHECK(!a.joinable());
    CHECK(b.joinable());

    // 移动赋值（目标对象持有未 join 的线程时先自动 join）
    SmallThread c([&] { counter.fetch_add(1); });
    c = std::move(b);
    CHECK(!b.joinable());
    CHECK(c.joinable());
    c.join();
    CHECK(!c.joinable());
    CHECK_EQ(counter.load(), 2);
}

// 同一对象重复 start（start 内部先 join）：多次执行任务
TEST_CASE(test_small_thread_repeated_start) {
    std::atomic<int> runs{0};
    SmallThread t;
    CHECK(!t.joinable());
    for (int i = 0; i < 5; ++i) {
        t.start([&] { runs.fetch_add(1); });
        CHECK(t.joinable());
    }
    t.join();
    CHECK_EQ(runs.load(), 5);

    // join 后再次 start 仍然可用
    t.start([&] { runs.fetch_add(10); });
    t.join();
    CHECK_EQ(runs.load(), 15);
}

// 并发多线程（8 个）各自累加，结果正确
TEST_CASE(test_small_thread_concurrent_accumulate) {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 10000;
    std::atomic<std::int64_t> total{0};

    {
        std::vector<SmallThread> threads;
        threads.reserve(kThreads);
        for (int i = 0; i < kThreads; ++i) {
            threads.emplace_back([&] {
                for (int j = 0; j < kPerThread; ++j) total.fetch_add(1);
            });
        }
        for (auto& t : threads) CHECK(t.joinable());
        for (auto& t : threads) t.join();
    }
    CHECK_EQ(total.load(), std::int64_t(kThreads) * kPerThread);
}
