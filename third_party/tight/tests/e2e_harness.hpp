#pragma once

// 端到端测试 harness：可脚本化的内存链路（丢包/重复/延迟/乱序）。
//
// 两个 TightTransport 各持一个 ScriptedSocket 并交叉 attach 后即互通，
// 不经任何真实 socket。链路策略在 send_to 时按包序号判定（确定性，
// 可复现），pump 线程到点投递 —— 收发解耦，与真实网络语义一致。
//
// 典型用法：
//   auto sa = std::make_shared<ScriptedSocket>();
//   auto sb = std::make_shared<ScriptedSocket>();
//   TightTransport a(cfg_a, sa), b(cfg_b, sb);
//   sa->attach(&b, {"127.0.0.1", PA});   // A 发出 → 注入 B，来源 = A
//   sb->attach(&a, {"127.0.0.1", PB});
//   sa->start(); sb->start();
//   a.start(); b.start();
//   sa->set_policy({.drop = drop_every(3)});   // A→B 方向每 3 包丢 1
//   ...测试结束后：sa->stop(); sb->stop(); a.stop(); b.stop();

#include "tight/tight.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace e2e {

using namespace std::chrono_literals;

class ScriptedSocket : public tight::ISocket {
public:
    // 链路策略（全部可选，默认零丢包零延迟直通了）。
    struct Policy {
        // 丢包判定：参数 = (包序号, 载荷大小)，返回 true 即丢（对上层表现为发送成功）
        std::function<bool(std::uint64_t index, std::size_t size)> drop;
        int dup_every = 0;                       // >0：每 n 包重复投递一次
        std::chrono::milliseconds delay{0};      // 固定延迟
        std::chrono::milliseconds jitter{0};     // 确定性伪随机附加延迟 [0, jitter)，制造乱序
        // 逐包自定义延迟（设置后覆盖 delay/jitter），参数 = (包序号, 载荷大小)
        std::function<std::chrono::milliseconds(std::uint64_t index, std::size_t size)> delay_fn;
    };

    ~ScriptedSocket() override { stop(); }

    // target = 对端 transport（报文注入对象），from = 本端逻辑地址
    void attach(tight::TightTransport* target, tight::NetAddress from) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_target = target;
        m_from = std::move(from);
    }

    void set_policy(Policy p) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_policy = std::move(p);
    }

    void start() {
        {
            std::lock_guard<std::mutex> lk(m_mu);
            if (m_running) return;
            m_running = true;
        }
        m_thread = std::thread([this] { pump(); });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_running = false;
        }
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

    // 经 send_to 发出的报文总数（不含被丢/重复副本）
    std::uint64_t sent_count() const { return m_counter.load(); }

    bool send_to(const tight::NetAddress& /*to*/, const std::uint8_t* data,
                 std::size_t size) override {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_running) return false;
        std::uint64_t idx = m_counter.fetch_add(1);
        if (m_policy.drop && m_policy.drop(idx, size)) return true;
        auto d = m_policy.delay_fn ? m_policy.delay_fn(idx, size)
                                   : m_policy.delay + jitter_of(idx);
        enqueue_locked(d, data, size);
        if (m_policy.dup_every > 0 && (idx + 1) % m_policy.dup_every == 0) {
            enqueue_locked(d, data, size);
        }
        return true;
    }

private:
    struct Item {
        std::chrono::steady_clock::time_point at;
        std::vector<std::uint8_t> bytes;
    };

    // 确定性伪随机（同 index 同结果，测试可复现）
    std::chrono::milliseconds jitter_of(std::uint64_t idx) const {
        auto j = m_policy.jitter.count();
        if (j <= 0) return std::chrono::milliseconds{0};
        return std::chrono::milliseconds{
            static_cast<long long>((idx * 2654435761ULL) % static_cast<std::uint64_t>(j))};
    }

    void enqueue_locked(std::chrono::milliseconds delay, const std::uint8_t* data,
                        std::size_t size) {
        m_queue.push_back({std::chrono::steady_clock::now() + delay,
                           std::vector<std::uint8_t>(data, data + size)});
        m_cv.notify_one();
    }

    void pump() {
        for (;;) {
            Item item;
            tight::TightTransport* target = nullptr;
            tight::NetAddress from;
            {
                std::unique_lock<std::mutex> lk(m_mu);
                for (;;) {
                    if (!m_running && m_queue.empty()) return;
                    if (m_queue.empty()) {
                        m_cv.wait(lk);
                        continue;
                    }
                    // 取最早到期项（队列短，O(n) 扫描足够）
                    auto best = m_queue.begin();
                    for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
                        if (it->at < best->at) best = it;
                    }
                    auto now = std::chrono::steady_clock::now();
                    if (best->at <= now) {
                        item = std::move(*best);
                        m_queue.erase(best);
                        break;
                    }
                    m_cv.wait_until(lk, best->at);
                }
                target = m_target;
                from = m_from;
            }
            if (target) {
                target->inject_packet(from, item.bytes.data(), item.bytes.size());
            }
        }
    }

    std::mutex m_mu;
    std::condition_variable m_cv;
    std::deque<Item> m_queue;
    Policy m_policy;
    tight::TightTransport* m_target{nullptr};
    tight::NetAddress m_from;
    std::atomic<std::uint64_t> m_counter{0};
    bool m_running{false};
    std::thread m_thread;
};

// ---- 常用丢包策略 ----

// 每 n 包丢 1（idx 从 0 起）
inline std::function<bool(std::uint64_t, std::size_t)> drop_every(int n) {
    return [n](std::uint64_t idx, std::size_t) { return (idx + 1) % n == 0; };
}

// 只丢前 n 包（idx 从 0 起）
inline std::function<bool(std::uint64_t, std::size_t)> drop_first(int n) {
    return [n](std::uint64_t idx, std::size_t) { return idx < static_cast<std::uint64_t>(n); };
}

// ---- 工具 ----

inline bool wait_for(const std::function<bool()>& pred,
                     std::chrono::milliseconds timeout = 10s) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

// 端到端用快速配置：报告/心跳/重传周期收紧，关测速列车
inline tight::TightConfig make_config(const std::string& id, std::uint16_t port) {
    tight::TightConfig cfg;
    cfg.id = id;
    cfg.bind = tight::NetAddress{"127.0.0.1", port};
    cfg.speed_test_enabled = false;
    cfg.heartbeat = 500ms;
    cfg.report_interval = 100ms;
    cfg.retransmit_timeout = 200ms;
    cfg.flush_interval = 5ms;
    return cfg;
}

// 交叉连接两个 transport（调用方负责 start/stop 与回调设置）
struct Link {
    std::shared_ptr<ScriptedSocket> sa = std::make_shared<ScriptedSocket>();
    std::shared_ptr<ScriptedSocket> sb = std::make_shared<ScriptedSocket>();

    // A→B 方向策略（sa 携带）
    void set_ab_policy(ScriptedSocket::Policy p) { sa->set_policy(std::move(p)); }
    // B→A 方向策略（sb 携带）
    void set_ba_policy(ScriptedSocket::Policy p) { sb->set_policy(std::move(p)); }

    void attach(tight::TightTransport* a, tight::NetAddress addr_a,
                tight::TightTransport* b, tight::NetAddress addr_b) {
        sa->attach(b, addr_a);
        sb->attach(a, addr_b);
    }
    void start() { sa->start(); sb->start(); }
    void stop() { sa->stop(); sb->stop(); }
};

// 等待 transport 出现 Established/Online 状态的 peer（id 为空 = 任意）
inline bool wait_peer_up(tight::TightTransport& t, const std::string& id = "",
                         std::chrono::milliseconds timeout = 10s) {
    return wait_for([&] {
        for (const auto& ev : t.peers()) {
            if (!id.empty() && ev.id != id) continue;
            if (ev.state == tight::LinkState::Established ||
                ev.state == tight::LinkState::Online) {
                return true;
            }
        }
        return false;
    }, timeout);
}

} // namespace e2e
