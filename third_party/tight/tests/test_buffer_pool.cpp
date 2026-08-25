#include "test_framework.hpp"

#include "util/buffer_pool.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

using tight::tight_detail::PooledAllocator;
using tight::tight_detail::PooledBytes;
using tight::tight_detail::PoolFreeList;
using tight::tight_detail::kPoolBlockSize;
using tight::tight_detail::kPoolMaxFreePerThread;
using tight::tight_detail::pool_free_list;

// 小规模分配/释放后再次分配复用同一块（自由链表 LIFO）
TEST_CASE(test_pool_reuses_freed_block) {
    PooledAllocator<std::uint8_t> alloc;
    auto* p1 = alloc.allocate(1);
    CHECK(p1 != nullptr);
    alloc.deallocate(p1, 1);
    auto* p2 = alloc.allocate(1);
    CHECK_EQ(p2, p1);
    alloc.deallocate(p2, 1);
}

// n > 2048 的分配走 ::operator new，不进池（池大小不变）
TEST_CASE(test_pool_oversize_bypasses_pool) {
    PooledAllocator<std::uint8_t> alloc;
    PoolFreeList& list = pool_free_list();
    const std::size_t before = list.size;

    constexpr std::size_t kBig = kPoolBlockSize + 1024;
    auto* p = alloc.allocate(kBig);
    CHECK(p != nullptr);
    CHECK_EQ(list.size, before); // 分配不触碰池

    alloc.deallocate(p, kBig);
    CHECK_EQ(list.size, before); // 释放也不进池
}

// 与 std::vector 配合：push_back 增长跨 2048 边界，数据完整
TEST_CASE(test_pooled_bytes_growth_across_block) {
    PooledBytes v;
    constexpr std::size_t kTotal = kPoolBlockSize * 2 + 123;
    for (std::size_t i = 0; i < kTotal; ++i) {
        v.push_back(static_cast<std::uint8_t>(i * 31 + 7));
    }
    CHECK_EQ(v.size(), kTotal);
    for (std::size_t i = 0; i < kTotal; ++i) {
        CHECK_EQ(v[i], static_cast<std::uint8_t>(i * 31 + 7));
    }
}

// 自由链表上限 16：连续释放 20 块后池只留 16
TEST_CASE(test_pool_free_list_cap) {
    PooledAllocator<std::uint8_t> alloc;
    PoolFreeList& list = pool_free_list();

    // 分配 20 块：先抽空池（size 变 0），其余来自 ::operator new
    constexpr std::size_t kCount = kPoolMaxFreePerThread + 4; // 20
    void* ptrs[kCount];
    for (std::size_t i = 0; i < kCount; ++i) {
        ptrs[i] = alloc.allocate(1);
        CHECK(ptrs[i] != nullptr);
    }
    CHECK_EQ(list.size, std::size_t(0));

    // 全部释放：前 16 块入池，剩余 4 块走 ::operator delete
    for (std::size_t i = 0; i < kCount; ++i) {
        alloc.deallocate(static_cast<std::uint8_t*>(ptrs[i]), 1);
    }
    CHECK_EQ(list.size, kPoolMaxFreePerThread);

    // 再分配 16 块应恰好取到池中的 16 块（LIFO，逆序）
    for (std::size_t i = 0; i < kPoolMaxFreePerThread; ++i) {
        void* q = alloc.allocate(1);
        // 池中保留的是 ptrs[0..15]（后 4 块被 delete），LIFO 弹出
        CHECK_EQ(q, ptrs[kPoolMaxFreePerThread - 1 - i]);
    }
    CHECK_EQ(list.size, std::size_t(0));
}

// 跨线程释放：主线程分配的块在线程 B 释放，进 B 的池并可复用
TEST_CASE(test_pool_cross_thread_deallocate) {
    PooledAllocator<std::uint8_t> alloc;
    auto* p = alloc.allocate(1); // 主线程分配
    CHECK(p != nullptr);

    std::atomic<bool> ok{false};
    std::thread t([&] {
        // 新线程的 thread_local 自由链表初始为空
        PoolFreeList& b_list = pool_free_list();
        const std::size_t before = b_list.size;
        alloc.deallocate(p, 1); // 在 B 线程释放，进 B 的池
        bool entered = (b_list.size == before + 1);
        void* q = alloc.allocate(1); // B 再分配，应拿回同一块
        ok = entered && (q == p);
        // q 留在 B 的池中随线程退出回收（设计如此）
        alloc.deallocate(static_cast<std::uint8_t*>(q), 1);
    });
    t.join();
    CHECK(ok);
}

// PooledBytes 拷贝/移动语义
TEST_CASE(test_pooled_bytes_copy_move) {
    PooledBytes a;
    for (std::uint8_t i = 0; i < 100; ++i) a.push_back(i);

    // 拷贝构造：内容一致、互不影响
    PooledBytes b = a;
    CHECK_EQ(b.size(), a.size());
    CHECK(b == a);
    b[0] = 255;
    CHECK_NE(b[0], a[0]);

    // 拷贝赋值
    PooledBytes c;
    c = a;
    CHECK(c == a);

    // 移动构造：源为空、内容一致
    PooledBytes d = std::move(a);
    CHECK(a.empty());
    CHECK_EQ(d.size(), std::size_t(100));
    for (std::uint8_t i = 0; i < 100; ++i) CHECK_EQ(d[i], i);

    // 移动赋值：源为空、内容一致
    PooledBytes e;
    e.push_back(42);
    e = std::move(d);
    CHECK(d.empty());
    CHECK_EQ(e.size(), std::size_t(100));
    for (std::uint8_t i = 0; i < 100; ++i) CHECK_EQ(e[i], i);
}
