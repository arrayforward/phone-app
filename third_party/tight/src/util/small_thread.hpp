#pragma once

// 小栈线程：std::thread 的轻量替代，创建时可指定栈大小。
// Windows 下 std::thread 默认每线程 1MB 栈预留；精简模式（lite_mode）
// 用 64KB 小栈显著降低每线程的地址预留与提交。接口与 std::thread 的
// 常用子集（joinable/join）一致，析构自动 join（不会 terminate）。
// 非公开 API。

#include <cstddef>
#include <functional>
#include <utility>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace tight::tight_detail {

class SmallThread {
public:
    SmallThread() = default;

    // stack_bytes == 0 表示使用系统默认栈大小
    template <typename F>
    explicit SmallThread(F&& fn, std::size_t stack_bytes = 0) {
        start(std::forward<F>(fn), stack_bytes);
    }

    ~SmallThread() { join(); }

    SmallThread(const SmallThread&) = delete;
    SmallThread& operator=(const SmallThread&) = delete;

    SmallThread(SmallThread&& o) noexcept : m_handle(o.m_handle) {
        o.m_handle = kEmpty;
    }
    SmallThread& operator=(SmallThread&& o) noexcept {
        if (this != &o) {
            join();
            m_handle = o.m_handle;
            o.m_handle = kEmpty;
        }
        return *this;
    }

    bool joinable() const { return m_handle != kEmpty; }

    void join() {
        if (!joinable()) return;
#ifdef _WIN32
        WaitForSingleObject(m_handle, INFINITE);
        CloseHandle(m_handle);
#else
        pthread_join(m_handle, nullptr);
#endif
        m_handle = kEmpty;
    }

    template <typename F>
    void start(F&& fn, std::size_t stack_bytes = 0) {
        join();
        auto* heap_fn = new std::function<void()>(std::forward<F>(fn));
#ifdef _WIN32
        m_handle = reinterpret_cast<HANDLE>(_beginthreadex(
            nullptr, static_cast<unsigned>(stack_bytes), thunk, heap_fn, 0, nullptr));
        if (!m_handle) delete heap_fn;
#else
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        if (stack_bytes > 0) {
            pthread_attr_setstacksize(&attr, stack_bytes);
        }
        if (pthread_create(&m_handle, &attr, thunk, heap_fn) != 0) {
            m_handle = kEmpty;
            delete heap_fn;
        }
        pthread_attr_destroy(&attr);
#endif
    }

private:
#ifdef _WIN32
    using Handle = HANDLE;
    static constexpr Handle kEmpty = nullptr;
    static unsigned __stdcall thunk(void* arg) {
        auto* fn = static_cast<std::function<void()>*>(arg);
        (*fn)();
        delete fn;
        return 0;
    }
#else
    using Handle = pthread_t;
    static constexpr Handle kEmpty = 0;
    static void* thunk(void* arg) {
        auto* fn = static_cast<std::function<void()>*>(arg);
        (*fn)();
        delete fn;
        return nullptr;
    }
#endif
    Handle m_handle{kEmpty};
};

} // namespace tight::tight_detail
