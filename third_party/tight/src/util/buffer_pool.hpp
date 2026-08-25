#pragma once

// 固定块缓冲池分配器（内部使用，非公开 API）。
//
// 出站报文（线上 datagram）是最高频的短生命周期分配：每个分片一个，
// 经出站队列后即释放。用 thread_local 自由链表复用固定 2048B 块：
//   - 无锁（每线程自己的自由链表，分配/释放都是本地操作）
//   - 块大小统一 2048B（覆盖 mtu ≤ ~2000 的报文；超出回退 ::operator new）
//   - 每线程最多缓存 16 块（32KB），空闲块有界
// 跨线程释放安全：块在释放线程的链表上回收（内存无归属）。

#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

namespace tight::tight_detail {

inline constexpr std::size_t kPoolBlockSize = 2048;
inline constexpr std::size_t kPoolMaxFreePerThread = 16;

// 每线程自由链表（函数内 thread_local，随线程生命周期）。
// 用 POD 定长数组而非 std::vector：POD 无线程退出析构，规避 MinGW
// emutls 下 thread_local 带析构对象在线程退出时被重复析构/悬垂释放
// 导致的堆损坏（0xc0000374）；块原本就随线程退出泄漏（不回收），
// 行为一致。
struct PoolFreeList {
    void* blocks[kPoolMaxFreePerThread];
    std::size_t size{0};
};

inline PoolFreeList& pool_free_list() {
    thread_local PoolFreeList list{};
    return list;
}

template <typename T>
class PooledAllocator {
public:
    using value_type = T;

    PooledAllocator() = default;
    template <typename U>
    PooledAllocator(const PooledAllocator<U>&) {}

    T* allocate(std::size_t n) {
        if (n <= kPoolBlockSize / sizeof(T)) {
            auto& list = pool_free_list();
            if (list.size > 0) {
                return static_cast<T*>(list.blocks[--list.size]);
            }
            // 池块统一按 kPoolBlockSize 分配（vector 容量仍记 n）
            return static_cast<T*>(::operator new(kPoolBlockSize));
        }
        return static_cast<T*>(::operator new(n * sizeof(T)));
    }

    void deallocate(T* p, std::size_t n) noexcept {
        if (n <= kPoolBlockSize / sizeof(T)) {
            auto& list = pool_free_list();
            if (list.size < kPoolMaxFreePerThread) {
                list.blocks[list.size++] = p;
                return;
            }
        }
        ::operator delete(p);
    }

    template <typename U>
    bool operator==(const PooledAllocator<U>&) const { return true; }
    template <typename U>
    bool operator!=(const PooledAllocator<U>&) const { return false; }
};

using PooledBytes = std::vector<std::uint8_t, PooledAllocator<std::uint8_t>>;

} // namespace tight::tight_detail
