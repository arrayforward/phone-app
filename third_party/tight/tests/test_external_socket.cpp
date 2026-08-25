// 外部 socket 模式（ISocket + inject_packet）端到端测试：
// 两个 TightTransport 通过内存 loopback socket 互联（不经任何真实
// socket），验证握手、双向消息投递与 local_port() 标识。
//
// LoopbackSocket 的 send_to 只入队，由独立 pump 线程注入对端——与真实
// 场景一致（收发解耦），避免在 tight 内部持锁发送时同步重入对端。

#include "test_framework.hpp"

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
#include <vector>

using namespace tight;
using namespace std::chrono_literals;

namespace {

class LoopbackSocket : public ISocket {
public:
    ~LoopbackSocket() override { stop(); }

    void attach(TightTransport* target, NetAddress from) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_target = target;
        m_from = std::move(from);
    }

    void start() {
        {
            std::lock_guard<std::mutex> lk(m_mu);
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

    bool send_to(const NetAddress& /*to*/, const std::uint8_t* data,
                 std::size_t size) override {
        {
            std::lock_guard<std::mutex> lk(m_mu);
            if (!m_running) return false;
            m_queue.emplace_back(data, data + size);
        }
        m_cv.notify_one();
        return true;
    }

private:
    void pump() {
        for (;;) {
            std::vector<std::uint8_t> d;
            TightTransport* target = nullptr;
            NetAddress from;
            {
                std::unique_lock<std::mutex> lk(m_mu);
                m_cv.wait(lk, [&] { return !m_running || !m_queue.empty(); });
                if (!m_running && m_queue.empty()) return;
                d = std::move(m_queue.front());
                m_queue.pop_front();
                target = m_target;
                from = m_from;
            }
            if (target) target->inject_packet(from, d.data(), d.size());
        }
    }

    std::mutex m_mu;
    std::condition_variable m_cv;
    std::deque<std::vector<std::uint8_t>> m_queue;
    TightTransport* m_target{nullptr};
    NetAddress m_from;
    bool m_running{false};
    std::thread m_thread;
};

bool wait_for(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 5s) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

TightConfig make_config(const std::string& id, std::uint16_t port) {
    TightConfig cfg;
    cfg.id = id;
    cfg.bind = NetAddress{"127.0.0.1", port};
    cfg.speed_test_enabled = false;  // 内存回环不需要测速列车
    return cfg;
}

} // namespace

// 默认（内部 UDP socket）模式端到端冒烟：两个实例经 localhost UDP
// 握手并投递消息，验证 start/connect/send/stop 全生命周期。
TEST_CASE(internal_udp_loopback_smoke) {
    TightTransport a(make_config("A", 0));
    TightTransport b(make_config("B", 0));

    std::mutex b_mu;
    std::vector<std::pair<std::string, Bytes>> b_msgs;
    b.set_message_callback([&](const std::string& peer, Bytes payload) {
        std::lock_guard<std::mutex> lk(b_mu);
        b_msgs.emplace_back(peer, std::move(payload));
    });

    CHECK(a.start());
    CHECK(b.start());
    CHECK(a.connect(RemotePeer{"B", NetAddress{"127.0.0.1", b.local_port()}}));

    bool a_up = wait_for([&] {
        for (const auto& ev : a.peers()) {
            if (ev.id == "B" &&
                (ev.state == LinkState::Established || ev.state == LinkState::Online)) {
                return true;
            }
        }
        return false;
    });
    CHECK(a_up);

    Bytes hello{'h', 'e', 'l', 'l', 'o'};
    CHECK(a.send("B", hello));
    CHECK(wait_for([&] {
        std::lock_guard<std::mutex> lk(b_mu);
        return !b_msgs.empty();
    }));
    a.stop();
    b.stop();
}

TEST_CASE(external_socket_handshake_and_bidirectional_messages) {
    auto sock_a = std::make_shared<LoopbackSocket>();
    auto sock_b = std::make_shared<LoopbackSocket>();

    TightTransport a(make_config("A", 10001), sock_a);
    TightTransport b(make_config("B", 10002), sock_b);

    // B 记录收到的消息（peer_id + payload）
    std::mutex b_mu;
    std::vector<std::pair<std::string, Bytes>> b_msgs;
    b.set_message_callback([&](const std::string& peer, Bytes payload) {
        std::lock_guard<std::mutex> lk(b_mu);
        b_msgs.emplace_back(peer, std::move(payload));
    });
    // A 记录收到的消息
    std::mutex a_mu;
    std::vector<std::pair<std::string, Bytes>> a_msgs;
    a.set_message_callback([&](const std::string& peer, Bytes payload) {
        std::lock_guard<std::mutex> lk(a_mu);
        a_msgs.emplace_back(peer, std::move(payload));
    });

    sock_a->attach(&b, NetAddress{"127.0.0.1", 10001});  // A 发出 → 注入 B，来源 = A
    sock_b->attach(&a, NetAddress{"127.0.0.1", 10002});  // B 发出 → 注入 A，来源 = B
    sock_a->start();
    sock_b->start();

    CHECK(a.start());
    CHECK(b.start());
    // 外部 socket 模式：local_port 为配置标识值（无真实绑定）
    CHECK_EQ(a.local_port(), 10001);
    CHECK_EQ(b.local_port(), 10002);

    CHECK(a.connect(RemotePeer{"B", NetAddress{"127.0.0.1", 10002}}));

    // 等待 A 侧链路建立
    bool a_up = wait_for([&] {
        for (const auto& ev : a.peers()) {
            if (ev.id == "B" &&
                (ev.state == LinkState::Established || ev.state == LinkState::Online)) {
                return true;
            }
        }
        return false;
    });
    CHECK(a_up);
    // 等待 B 侧发现 A（匿名 peer）
    bool b_up = wait_for([&] {
        for (const auto& ev : b.peers()) {
            if (ev.state == LinkState::Established || ev.state == LinkState::Online) {
                return true;
            }
        }
        return false;
    });
    CHECK(b_up);

    // A → B
    Bytes hello{'h', 'e', 'l', 'l', 'o'};
    CHECK(a.send("B", hello));
    CHECK(wait_for([&] {
        std::lock_guard<std::mutex> lk(b_mu);
        return !b_msgs.empty();
    }));
    {
        std::lock_guard<std::mutex> lk(b_mu);
        CHECK_EQ(b_msgs.size(), 1U);
        CHECK(b_msgs[0].second == hello);
    }

    // B → A（B 侧 peer 为匿名 id，从 peers() 取）
    std::string anon_id;
    for (const auto& ev : b.peers()) {
        if (ev.state == LinkState::Established || ev.state == LinkState::Online) {
            anon_id = ev.id;
            break;
        }
    }
    CHECK(!anon_id.empty());
    Bytes world{'w', 'o', 'r', 'l', 'd'};
    CHECK(b.send(anon_id, world));
    CHECK(wait_for([&] {
        std::lock_guard<std::mutex> lk(a_mu);
        return !a_msgs.empty();
    }));
    {
        std::lock_guard<std::mutex> lk(a_mu);
        CHECK_EQ(a_msgs.size(), 1U);
        CHECK(a_msgs[0].first == "B");
        CHECK(a_msgs[0].second == world);
    }

    // 先停 pump（不再注入），再停 transport
    sock_a->stop();
    sock_b->stop();
    a.stop();
    b.stop();
}

TEST_CASE(external_socket_null_falls_back_to_internal) {
    // socket 为空时等价默认构造（内部 UDP socket，bind 端口 0）
    TightTransport t(make_config("Solo", 0), nullptr);
    CHECK(t.start());
    CHECK(t.local_port() != 0);  // 内部模式：内核分配真实端口
    t.stop();
}

