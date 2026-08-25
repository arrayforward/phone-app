// 端到端模式/边界测试：lite 模式（全程 + 运行时切换）、按通道排空、
// clear_outbound 恢复、消息长度上限、关闭加密/FEC、测速列车、
// send_video 关键帧路径、未连接发送语义。
//
// 全部经 e2e_harness.hpp 的 ScriptedSocket/Link 内存链路（零丢包零延迟
// 直通），不经真实 socket。每用例独立端口对，确定性、可复现。

#include "test_framework.hpp"

#include "e2e_harness.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace tight;
using namespace std::chrono_literals;

namespace {

// 接收侧消息收集器：回调在 harness pump 线程内同步触发，
// 只加锁入队，禁止阻塞。
struct MsgSink {
    std::mutex mu;
    std::vector<std::pair<std::string, Bytes>> msgs;

    TightTransport::MessageCallback callback() {
        return [this](const std::string& peer, Bytes payload) {
            std::lock_guard<std::mutex> lk(mu);
            msgs.emplace_back(peer, std::move(payload));
        };
    }

    std::size_t count() {
        std::lock_guard<std::mutex> lk(mu);
        return msgs.size();
    }

    bool has_payload(const Bytes& p) {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& kv : msgs) {
            if (kv.second == p) return true;
        }
        return false;
    }
};

// B 侧对端是匿名 id：从 peers() 取第一个 Established/Online 的 peer id。
std::string first_up_peer(TightTransport& t) {
    for (const auto& ev : t.peers()) {
        if (ev.state == LinkState::Established || ev.state == LinkState::Online) {
            return ev.id;
        }
    }
    return {};
}

bool wait_payload(MsgSink& sink, const Bytes& p, std::chrono::milliseconds timeout = 5s) {
    return e2e::wait_for([&] { return sink.has_payload(p); }, timeout);
}

// 构造避免 0x01/0x02/0x03 开头的载荷（这些首字节是 file/data 内部 tag，
// 会被接收端 deliver_message 拦截，不走应用 message_callback）。
Bytes make_payload(std::uint8_t marker, std::size_t size) {
    Bytes p(size, static_cast<std::uint8_t>(0xA0u ^ marker));
    if (!p.empty()) p[0] = marker;  // marker 均取 >= 0x10
    return p;
}

} // namespace

// 1. lite 模式全程：两端 lite_mode=true（LiteProfile::Video），
//    握手 + 双向消息正常。
TEST_CASE(lite_mode_end_to_end) {
    e2e::Link link;
    auto cfg_a = e2e::make_config("A", 12001);
    auto cfg_b = e2e::make_config("B", 12002);
    cfg_a.lite_mode = true;
    cfg_a.lite_profile = LiteProfile::Video;
    cfg_b.lite_mode = true;
    cfg_b.lite_profile = LiteProfile::Video;

    TightTransport a(cfg_a, link.sa);
    TightTransport b(cfg_b, link.sb);

    MsgSink a_sink, b_sink;
    a.set_message_callback(a_sink.callback());
    b.set_message_callback(b_sink.callback());

    link.attach(&a, {"127.0.0.1", 12001}, &b, {"127.0.0.1", 12002});
    link.start();
    CHECK(a.start());
    CHECK(b.start());
    CHECK(a.lite_mode());
    CHECK(b.lite_mode());
    CHECK(a.connect(RemotePeer{"B", NetAddress{"127.0.0.1", 12002}}));
    CHECK(e2e::wait_peer_up(a, "B"));
    CHECK(e2e::wait_peer_up(b));

    // A → B
    auto p1 = make_payload(0x11, 100);
    CHECK(a.send("B", p1));
    CHECK(wait_payload(b_sink, p1));

    // B → A（匿名 id）
    std::string anon = first_up_peer(b);
    CHECK(!anon.empty());
    auto p2 = make_payload(0x12, 100);
    CHECK(b.send(anon, p2));
    CHECK(wait_payload(a_sink, p2));

    link.stop();
    a.stop();
    b.stop();
}

// 2. lite 运行时切换：普通模式建链发消息 → set_lite_mode(true) → 发消息
//    → set_lite_mode(false) → 再发消息，全程 B 都能收到。
TEST_CASE(lite_mode_runtime_switch) {
    e2e::Link link;
    auto cfg_a = e2e::make_config("A", 12101);
    auto cfg_b = e2e::make_config("B", 12102);

    TightTransport a(cfg_a, link.sa);
    TightTransport b(cfg_b, link.sb);

    MsgSink b_sink;
    b.set_message_callback(b_sink.callback());

    link.attach(&a, {"127.0.0.1", 12101}, &b, {"127.0.0.1", 12102});
    link.start();
    CHECK(a.start());
    CHECK(b.start());
    CHECK(!a.lite_mode());
    CHECK(a.connect(RemotePeer{"B", NetAddress{"127.0.0.1", 12102}}));
    CHECK(e2e::wait_peer_up(a, "B"));
    CHECK(e2e::wait_peer_up(b));

    // 普通模式：消息可达
    auto p1 = make_payload(0x21, 64);
    CHECK(a.send("B", p1));
    CHECK(wait_payload(b_sink, p1));

    // 切到 lite：消息仍可达
    a.set_lite_mode(true);
    CHECK(a.lite_mode());
    auto p2 = make_payload(0x22, 64);
    CHECK(a.send("B", p2));
    CHECK(wait_payload(b_sink, p2));

    // 切回普通：消息仍可达
    a.set_lite_mode(false);
    CHECK(!a.lite_mode());
    auto p3 = make_payload(0x23, 64);
    CHECK(a.send("B", p3));
    CHECK(wait_payload(b_sink, p3));

    link.stop();
    a.stop();
    b.stop();
}

// 3. 按通道排空：drain_channel(0)（默认 100ms）期间 ch0 消息不达、
//    ch1 消息必达；排空期结束后新的 ch0 消息恢复可达。
TEST_CASE(drain_channel_drops_only_drained_channel) {
    e2e::Link link;
    auto cfg_a = e2e::make_config("A", 12201);
    auto cfg_b = e2e::make_config("B", 12202);

    TightTransport a(cfg_a, link.sa);
    TightTransport b(cfg_b, link.sb);

    MsgSink b_sink;
    b.set_message_callback(b_sink.callback());

    link.attach(&a, {"127.0.0.1", 12201}, &b, {"127.0.0.1", 12202});
    link.start();
    CHECK(a.start());
    CHECK(b.start());
    CHECK(a.connect(RemotePeer{"B", NetAddress{"127.0.0.1", 12202}}));
    CHECK(e2e::wait_peer_up(a, "B"));
    CHECK(e2e::wait_peer_up(b));

    // 进入 ch0 排空期（默认 100ms），立即发 ch0 + ch1 各一条
    a.drain_channel(0);
    auto p0_during = make_payload(0x31, 64);
    auto p1_during = make_payload(0x32, 64);
    CHECK(a.send_channel("B", p0_during, 0));  // 入队成功（出队时被排空丢弃）
    CHECK(a.send_channel("B", p1_during, 1));

    // ch1 不受排空影响，必达
    CHECK(wait_payload(b_sink, p1_during));

    // ch0 排空期消息不达（等远超排空期 + 无重传兜底：channel_reliable
    // 默认全 false，ch0 缺口立即跳过）
    std::this_thread::sleep_for(500ms);
    CHECK(!b_sink.has_payload(p0_during));

    // 排空期（100ms）早已结束，新 ch0 消息恢复可达
    auto p0_after = make_payload(0x33, 64);
    CHECK(a.send_channel("B", p0_after, 0));
    CHECK(wait_payload(b_sink, p0_after));

    link.stop();
    a.stop();
    b.stop();
}

// 4. clear_outbound 后恢复：连发多条后 clear_outbound() 不 crash、
//    outbound_queue_size() 可用，之后再发的消息能到。
TEST_CASE(clear_outbound_then_recover) {
    e2e::Link link;
    auto cfg_a = e2e::make_config("A", 12301);
    auto cfg_b = e2e::make_config("B", 12302);

    TightTransport a(cfg_a, link.sa);
    TightTransport b(cfg_b, link.sb);

    MsgSink b_sink;
    b.set_message_callback(b_sink.callback());

    link.attach(&a, {"127.0.0.1", 12301}, &b, {"127.0.0.1", 12302});
    link.start();
    CHECK(a.start());
    CHECK(b.start());
    CHECK(a.connect(RemotePeer{"B", NetAddress{"127.0.0.1", 12302}}));
    CHECK(e2e::wait_peer_up(a, "B"));
    CHECK(e2e::wait_peer_up(b));

    // 连发多条（可能被 clear_outbound 丢弃，不断言其到达）
    for (int i = 0; i < 10; ++i) {
        a.send("B", make_payload(static_cast<std::uint8_t>(0x40 + i), 256));
    }
    a.clear_outbound();  // 不 crash
    // outbound_queue_size() 可用（只验证可调用、返回 sane 值）
    CHECK(a.outbound_queue_size() <= 65536U * 2);

    // clear 之后再发的消息必须能到
    auto marker = make_payload(0x4F, 128);
    CHECK(a.send("B", marker));
    CHECK(wait_payload(b_sink, marker));

    link.stop();
    a.stop();
    b.stop();
}

// 5. 消息长度上限：max_message_bytes 默认 64KB——70KB 返回 false；
//    1KB 返回 true 且能到。
TEST_CASE(max_message_bytes_boundary) {
    e2e::Link link;
    auto cfg_a = e2e::make_config("A", 12401);
    auto cfg_b = e2e::make_config("B", 12402);
    CHECK_EQ(cfg_a.max_message_bytes, 64U * 1024U);  // 默认 64KB

    TightTransport a(cfg_a, link.sa);
    TightTransport b(cfg_b, link.sb);

    MsgSink b_sink;
    b.set_message_callback(b_sink.callback());

    link.attach(&a, {"127.0.0.1", 12401}, &b, {"127.0.0.1", 12402});
    link.start();
    CHECK(a.start());
    CHECK(b.start());
    CHECK(a.connect(RemotePeer{"B", NetAddress{"127.0.0.1", 12402}}));
    CHECK(e2e::wait_peer_up(a, "B"));
    CHECK(e2e::wait_peer_up(b));

    // 超限（70KB > 64KB）：发送拒绝
    CHECK(!a.send("B", make_payload(0x51, 70 * 1024)));

    // 限内（1KB）：接受且可达
    auto p = make_payload(0x52, 1024);
    CHECK(a.send("B", p));
    CHECK(wait_payload(b_sink, p));

    link.stop();
    a.stop();
    b.stop();
}

// 6. 关闭加密：两端 encryption_enabled=false，握手 + 消息正常。
TEST_CASE(encryption_disabled_end_to_end) {
    e2e::Link link;
    auto cfg_a = e2e::make_config("A", 12501);
    auto cfg_b = e2e::make_config("B", 12502);
    cfg_a.encryption_enabled = false;
    cfg_b.encryption_enabled = false;

    TightTransport a(cfg_a, link.sa);
    TightTransport b(cfg_b, link.sb);

    MsgSink b_sink;
    b.set_message_callback(b_sink.callback());

    link.attach(&a, {"127.0.0.1", 12501}, &b, {"127.0.0.1", 12502});
    link.start();
    CHECK(a.start());
    CHECK(b.start());
    CHECK(a.connect(RemotePeer{"B", NetAddress{"127.0.0.1", 12502}}));
    CHECK(e2e::wait_peer_up(a, "B"));
    CHECK(e2e::wait_peer_up(b));

    auto p = make_payload(0x61, 200);
    CHECK(a.send("B", p));
    CHECK(wait_payload(b_sink, p));

    link.stop();
    a.stop();
    b.stop();
}

// 7. 关闭 FEC：两端 fec_enabled=false，无丢包下消息正常（含多分片：
//    30KB > mtu 1350 → ~25 个分片）。
TEST_CASE(fec_disabled_multifragment) {
    e2e::Link link;
    auto cfg_a = e2e::make_config("A", 12601);
    auto cfg_b = e2e::make_config("B", 12602);
    cfg_a.fec_enabled = false;
    cfg_b.fec_enabled = false;

    TightTransport a(cfg_a, link.sa);
    TightTransport b(cfg_b, link.sb);

    MsgSink b_sink;
    b.set_message_callback(b_sink.callback());

    link.attach(&a, {"127.0.0.1", 12601}, &b, {"127.0.0.1", 12602});
    link.start();
    CHECK(a.start());
    CHECK(b.start());
    CHECK(a.connect(RemotePeer{"B", NetAddress{"127.0.0.1", 12602}}));
    CHECK(e2e::wait_peer_up(a, "B"));
    CHECK(e2e::wait_peer_up(b));

    auto p = make_payload(0x71, 30 * 1024);
    CHECK(a.send("B", p));
    CHECK(wait_payload(b_sink, p));

    link.stop();
    a.stop();
    b.stop();
}

// 8. 测速开启：两端 speed_test_enabled=true（默认 100KB 探测列车），
//    链路上线 + 消息正常（内存链路上探测列车很快完成）。
TEST_CASE(speed_test_enabled_link_up) {
    e2e::Link link;
    auto cfg_a = e2e::make_config("A", 12701);
    auto cfg_b = e2e::make_config("B", 12702);
    cfg_a.speed_test_enabled = true;
    cfg_b.speed_test_enabled = true;
    CHECK_EQ(cfg_a.speed_test_bytes, 100U * 1024U);  // 默认 100KB 列车

    TightTransport a(cfg_a, link.sa);
    TightTransport b(cfg_b, link.sb);

    MsgSink b_sink;
    b.set_message_callback(b_sink.callback());

    link.attach(&a, {"127.0.0.1", 12701}, &b, {"127.0.0.1", 12702});
    link.start();
    CHECK(a.start());
    CHECK(b.start());
    CHECK(a.connect(RemotePeer{"B", NetAddress{"127.0.0.1", 12702}}));
    CHECK(e2e::wait_peer_up(a, "B"));
    CHECK(e2e::wait_peer_up(b));

    auto p = make_payload(0x81, 128);
    CHECK(a.send("B", p));
    CHECK(wait_payload(b_sink, p));

    link.stop();
    a.stop();
    b.stop();
}

// 9. send_video 关键帧路径：keyframe=true/false 均正常投递
//    （接收行为与 send 一致，走 message_callback）。
TEST_CASE(send_video_keyframe_path) {
    e2e::Link link;
    auto cfg_a = e2e::make_config("A", 12801);
    auto cfg_b = e2e::make_config("B", 12802);

    TightTransport a(cfg_a, link.sa);
    TightTransport b(cfg_b, link.sb);

    MsgSink b_sink;
    b.set_message_callback(b_sink.callback());

    link.attach(&a, {"127.0.0.1", 12801}, &b, {"127.0.0.1", 12802});
    link.start();
    CHECK(a.start());
    CHECK(b.start());
    CHECK(a.connect(RemotePeer{"B", NetAddress{"127.0.0.1", 12802}}));
    CHECK(e2e::wait_peer_up(a, "B"));
    CHECK(e2e::wait_peer_up(b));

    auto key = make_payload(0x91, 300);
    CHECK(a.send_video("B", key, /*keyframe=*/true));
    CHECK(wait_payload(b_sink, key));

    auto nonkey = make_payload(0x92, 300);
    CHECK(a.send_video("B", nonkey, /*keyframe=*/false));
    CHECK(wait_payload(b_sink, nonkey));

    link.stop();
    a.stop();
    b.stop();
}

// 10. 未连接发送：send 到不存在的 peer id——实际语义是入队成功返回
//     true（send_message 只查运行状态/长度/队列容量，不查 peer 是否存在；
//     未知 peer 的消息在 process_send_queue 查找失败时被静默丢弃）。
//     按实际语义断言 true；且此后正常发送不受影响。
TEST_CASE(send_to_unknown_peer_enqueued_then_dropped) {
    e2e::Link link;
    auto cfg_a = e2e::make_config("A", 12901);
    auto cfg_b = e2e::make_config("B", 12902);

    TightTransport a(cfg_a, link.sa);
    TightTransport b(cfg_b, link.sb);

    MsgSink b_sink;
    b.set_message_callback(b_sink.callback());

    link.attach(&a, {"127.0.0.1", 12901}, &b, {"127.0.0.1", 12902});
    link.start();
    CHECK(a.start());
    CHECK(b.start());
    CHECK(a.connect(RemotePeer{"B", NetAddress{"127.0.0.1", 12902}}));
    CHECK(e2e::wait_peer_up(a, "B"));
    CHECK(e2e::wait_peer_up(b));

    // 不存在的 peer：入队返回 true（随后在发送管线被静默丢弃）
    CHECK(a.send("NO_SUCH_PEER", make_payload(0xA1, 64)));

    // B 不应收到该消息
    std::this_thread::sleep_for(300ms);
    CHECK(!b_sink.has_payload(make_payload(0xA1, 64)));

    // 正常 peer 的发送不受影响
    auto p = make_payload(0xA2, 64);
    CHECK(a.send("B", p));
    CHECK(wait_payload(b_sink, p));

    link.stop();
    a.stop();
    b.stop();
}
