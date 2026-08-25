// 端到端协议测试：两个 TightTransport 经 e2e_harness 的 ScriptedSocket/Link
// 内存链路互联（确定性丢包/重复/延迟/乱序），验证：可靠通道 ARQ 抗丢包、
// 不可靠通道丢帧通知、纯 FEC 无重传恢复、send_data 恰好一次、send_file
// 抗丢包、命令通道保序，共 6 个用例。
//
// 注意：transport 的 deliver_message 按 payload 首字节 tag 识别 file/data
// 内部消息（0x01=manifest, 0x02=chunk, 0x03=data），与通道无关——应用层
// send() 的消息首字节须避开 0x01..0x03。本文件的 pattern_bytes 首字节为 0。
//
// 调试中确认的库行为（协议特性，测试侧规避，详见各注释）：
//  1) 首包基线：接收端序列基线由"第一个到达的数据包"设定（reassembler
//     m_seq_initialized），之前丢失的包不产生缺口、永不 NACK。
//  2) 尾部缺口：缺口只能被"序号更大的后到包"揭示；有限突发的尾包丢失
//     无揭示者，永不 NACK/重传（流场景由后续包自然揭示，有限突发不行）。
//     1/2 的规避：突发前后加热身/收尾消息（见 establish_baseline /
//     deliver_with_trailers）。
//
// 已修复的历史缺陷（本文件即回归测试）：控制包（Handshake/Online）与数据
// 包曾共用 m_pending 映射但序号计数器独立（都从 1 起步），控制 Ack 会误删
// 同号数据 pending（该数据报丢失后永不重传）、数据 NACK 会误重传控制包。
// 修复：控制挂账拆分为 m_control_pending，Ack 只消费控制挂账，Report 只
// 消费数据 m_pending。因此 connect_ab 不再需要握手后静置。

#include "test_framework.hpp"

#include "e2e_harness.hpp"

#include "tight/tight.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace tight;
using namespace std::chrono_literals;

namespace {

// ---- 通用工具 ----

Bytes pattern_bytes(std::size_t n) {
    Bytes v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<std::uint8_t>(i % 251);
    }
    return v;
}

// 消息收集器：回调在 harness pump 线程内同步触发，只做加锁入队。
struct MsgBox {
    std::mutex mu;
    std::vector<std::pair<std::string, Bytes>> msgs;

    TightTransport::MessageCallback callback() {
        return [this](const std::string& peer, Bytes payload) {
            std::lock_guard<std::mutex> lk(mu);
            msgs.emplace_back(peer, std::move(payload));
        };
    }
    std::size_t size() {
        std::lock_guard<std::mutex> lk(mu);
        return msgs.size();
    }
    bool contains(const Bytes& want) {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& m : msgs) {
            if (m.second == want) return true;
        }
        return false;
    }
};

// 两端 transport + 交叉链路的组合体。析构前须调用 stop()。
struct Pair {
    e2e::Link link;
    TightTransport a;
    TightTransport b;
    NetAddress addr_a;
    NetAddress addr_b;

    Pair(const TightConfig& ca, const TightConfig& cb)
        : a(ca, link.sa), b(cb, link.sb), addr_a(ca.bind), addr_b(cb.bind) {
        link.attach(&a, addr_a, &b, addr_b);
    }

    void start() {
        link.start();
        a.start();
        b.start();
    }
    // 结束顺序：先停链路 pump（不再注入），再停两端 transport
    void stop() {
        link.stop();
        a.stop();
        b.stop();
    }
    // A 发起连接并等待双侧链路建立；B 侧对端为匿名 id
    bool connect_ab(std::chrono::milliseconds timeout = 15s) {
        if (!a.connect(RemotePeer{"B", addr_b})) return false;
        return e2e::wait_peer_up(a, "B", timeout) && e2e::wait_peer_up(b, "", timeout);
    }
};

e2e::ScriptedSocket::Policy drop_policy(
    std::function<bool(std::uint64_t, std::size_t)> drop) {
    e2e::ScriptedSocket::Policy p;
    p.drop = std::move(drop);
    return p;
}

// 热身：发单字节 tag 的消息直到至少一条到达——让接收端序列基线在正式
// 突发之前建立（规避协议特性 1：首包丢失无缺口、静默丢失）。
bool establish_baseline(TightTransport& from, const std::string& peer, MsgBox& box,
                        std::uint8_t tag, int max_rounds = 15) {
    Bytes wu{tag};
    std::size_t before = box.size();
    for (int i = 0; i < max_rounds; ++i) {
        from.send(peer, Bytes(wu));
        if (e2e::wait_for([&] { return box.size() > before; }, 300ms)) return true;
    }
    return false;
}

// 收尾：反复发收尾消息（新序号）直到 want 到达——收尾包的更大序号揭示
// 尾部缺口，触发 NACK 重传（规避协议特性 2）。
bool deliver_with_trailers(TightTransport& from, const std::string& peer, MsgBox& box,
                           const Bytes& want, int max_rounds = 20) {
    Bytes tr{'T', 'R'};
    for (int i = 0; i < max_rounds; ++i) {
        from.send(peer, Bytes(tr));
        if (e2e::wait_for([&] { return box.contains(want); }, 400ms)) return true;
    }
    return box.contains(want);
}

// 稀疏确定性丢包：只丢第 nth 个数据分片（>1KB 大包，控制包不计），
// 模拟现网偶发单包丢失事件（现网丢包率上限 ~3%，不做高丢包率压测）。
std::function<bool(std::uint64_t, std::size_t)> drop_nth_large(int nth) {
    auto cnt = std::make_shared<std::uint64_t>(0);
    return [cnt, nth](std::uint64_t, std::size_t size) {
        return size > 1000 && ++(*cnt) == static_cast<std::uint64_t>(nth);
    };
}

} // namespace

// 1. 可靠通道抗丢包：channel_reliable[0]，A→B 稀疏丢包（drop_every(33)，
//    ~3% 现网上限），64KB 多分片消息靠 NACK 重传完整到达。
TEST_CASE(e2e_reliable_channel_survives_loss) {
    auto ca = e2e::make_config("A", 11001);
    auto cb = e2e::make_config("B", 11002);
    ca.channel_reliable[0] = true;
    cb.channel_reliable[0] = true;

    Pair p(ca, cb);
    MsgBox box;
    p.b.set_message_callback(box.callback());
    p.start();
    CHECK(p.connect_ab());
    // 握手完成后再施加丢包，只影响数据面
    p.link.set_ab_policy(drop_policy(e2e::drop_every(33)));

    // 热身建立序列基线（正式消息首个分片若被丢，基线未建会静默丢失）
    CHECK(establish_baseline(p.a, "B", box, 'W'));

    Bytes msg = pattern_bytes(64 * 1024);
    CHECK(p.a.send("B", Bytes(msg)));
    // 收尾消息揭示尾部缺口（消息末尾分片丢失时触发 NACK 重传）
    CHECK(deliver_with_trailers(p.a, "B", box, msg));
    CHECK(box.contains(msg));
    p.stop();
}

// 2. 不可靠通道丢失通知：retransmit/FEC 全关，数据分片每 2 片丢 1（按包
//    大小分类，控制包不受影响），多分片消息无法恢复，B 侧
//    message_loss_callback 触发且 channel=0。
TEST_CASE(e2e_unreliable_loss_notification) {
    auto ca = e2e::make_config("A", 11011);
    auto cb = e2e::make_config("B", 11012);
    ca.retransmit_enabled = false;
    cb.retransmit_enabled = false;
    ca.fec_enabled = false;
    cb.fec_enabled = false;

    Pair p(ca, cb);
    std::mutex loss_mu;
    std::vector<std::pair<std::string, std::uint8_t>> losses;
    p.b.set_message_loss_callback([&](const std::string& peer, std::uint8_t channel) {
        std::lock_guard<std::mutex> lk(loss_mu);
        losses.emplace_back(peer, channel);
    });
    MsgBox box;
    p.b.set_message_callback(box.callback());
    p.start();
    CHECK(p.connect_ab());
    // 共享大包计数器：丢偶数号数据分片（必不可恢复）；延迟 ≡3(mod4) 号
    // 数据分片 500ms（奇数号必幸存）——丢失判定（reassembler 的 loss_wait
    // 超时分支）只在"该消息又有分片到达"时评估，纯丢包下剩余分片永远不
    // 再到、超时永不检查；迟到分片（> 不可靠通道 loss_wait 250ms）到达时
    // 触发超时评估，确认丢失并回调。
    e2e::ScriptedSocket::Policy pol;
    auto large_cnt = std::make_shared<std::uint64_t>(0);
    pol.drop = [large_cnt](std::uint64_t, std::size_t size) {
        return size > 1000 && (++(*large_cnt) % 2) == 0;
    };
    pol.delay_fn = [large_cnt](std::uint64_t, std::size_t size) {
        // delay_fn 在 drop 之后对幸存包调用，*large_cnt 即本包的大包序号
        return (size > 1000 && (*large_cnt % 4) == 3) ? std::chrono::milliseconds(500)
                                                      : std::chrono::milliseconds(0);
    };
    p.link.set_ab_policy(pol);

    Bytes msg = pattern_bytes(16 * 1024);  // ~13 片，半数必丢且无法恢复
    CHECK(p.a.send("B", Bytes(msg)));
    bool got_loss = e2e::wait_for([&] {
        std::lock_guard<std::mutex> lk(loss_mu);
        return !losses.empty();
    });
    CHECK(got_loss);
    {
        std::lock_guard<std::mutex> lk(loss_mu);
        if (!losses.empty()) CHECK_EQ(losses[0].second, 0);
    }
    // 该消息不可能完整重组
    std::this_thread::sleep_for(300ms);
    CHECK_EQ(box.size(), 0U);
    p.stop();
}

// 3. FEC 无重传恢复：retransmit 关、FEC 开、channel_fec_extra[0]=2，
//    第 3 个数据分片确定性丢失（drop_nth_large），8KB 消息不重传也由
//    RS 校验片恢复完整到达。
TEST_CASE(e2e_fec_recovers_without_retransmit) {
    auto ca = e2e::make_config("A", 11021);
    auto cb = e2e::make_config("B", 11022);
    ca.retransmit_enabled = false;
    cb.retransmit_enabled = false;
    ca.fec_enabled = true;
    cb.fec_enabled = true;
    ca.channel_fec_extra[0] = 2;
    cb.channel_fec_extra[0] = 2;

    Pair p(ca, cb);
    MsgBox box;
    p.b.set_message_callback(box.callback());
    std::mutex loss_mu;
    std::size_t loss_count = 0;
    p.b.set_message_loss_callback([&](const std::string&, std::uint8_t) {
        std::lock_guard<std::mutex> lk(loss_mu);
        ++loss_count;
    });
    p.start();
    CHECK(p.connect_ab());
    p.link.set_ab_policy(drop_policy(drop_nth_large(3)));

    Bytes msg = pattern_bytes(8 * 1024);  // ~7 数据片 + 校验片
    CHECK(p.a.send("B", Bytes(msg)));
    // RS 重组按 message_id 进行、与序列基线/缺口无关，无需热身/收尾；
    // 单分片丢失远在校验能力内，也不应触发丢帧回调
    CHECK(e2e::wait_for([&] { return box.contains(msg); }));
    CHECK(box.contains(msg));
    std::this_thread::sleep_for(500ms);
    {
        std::lock_guard<std::mutex> lk(loss_mu);
        CHECK_EQ(loss_count, 0U);
    }
    p.stop();
}

// 4. send_data 恰好一次：channel_reliable[3]，A→B 重复+丢包并存，
//    20 条不同内容消息每条恰好交付一次（消息级去重）。
TEST_CASE(e2e_send_data_exactly_once) {
    auto ca = e2e::make_config("A", 11031);
    auto cb = e2e::make_config("B", 11032);
    ca.channel_reliable[3] = true;
    cb.channel_reliable[3] = true;
    // 关闭拥塞排空窗口：确定性丢包会被 AIMD 判成剧烈拥塞，fast 排空
    // （clear_outbound_impl）会清空整个出站队列（含 file/data 数据报），
    // 引入与本用例（ARQ 重传 + 消息级去重）无关的不确定性。
    ca.slowdown_window_ms = 0;
    cb.slowdown_window_ms = 0;

    Pair p(ca, cb);
    std::mutex data_mu;
    std::vector<Bytes> received;  // 全部交付（含热身/收尾）
    p.b.set_data_callback([&](const std::string&, Bytes data) {
        std::lock_guard<std::mutex> lk(data_mu);
        received.push_back(std::move(data));
    });
    p.start();
    CHECK(p.connect_ab());
    e2e::ScriptedSocket::Policy pol;
    pol.drop = e2e::drop_every(33);  // ~3% 丢包（现网上限）
    pol.dup_every = 10;              // 偶发重复包（每 10 包 1 次）
    p.link.set_ab_policy(pol);

    constexpr int kCount = 20;
    constexpr std::size_t kRealSize = 64;  // 正式消息 64B，热身/收尾 8B
    auto count_real = [&] {
        std::lock_guard<std::mutex> lk(data_mu);
        return std::count_if(received.begin(), received.end(),
                             [](const Bytes& d) { return d.size() == kRealSize; });
    };
    // 热身（8B 标记负载）建立序列基线
    Bytes wu(8, 0xAA);
    bool base = false;
    for (int i = 0; i < 15 && !base; ++i) {
        p.a.send_data("B", Bytes(wu));
        base = e2e::wait_for([&] {
            std::lock_guard<std::mutex> lk(data_mu);
            return !received.empty();
        }, 300ms);
    }
    CHECK(base);

    std::set<std::string> expected;
    for (int i = 0; i < kCount; ++i) {
        Bytes payload(kRealSize, static_cast<std::uint8_t>(i));
        payload[1] = static_cast<std::uint8_t>(i * 7 + 3);
        expected.insert(std::string(payload.begin(), payload.end()));
        CHECK(p.a.send_data("B", std::move(payload)));
    }
    // 收尾消息（8B）揭示尾部缺口
    Bytes tr(8, 0xBB);
    for (int i = 0; i < 20 && count_real() < kCount; ++i) {
        p.a.send_data("B", Bytes(tr));
        e2e::wait_for([&] { return count_real() >= kCount; }, 400ms);
    }
    CHECK_EQ(count_real(), kCount);
    // 等一会抓潜在重复投递（去重窗口内不该再有交付）
    std::this_thread::sleep_for(500ms);
    {
        std::lock_guard<std::mutex> lk(data_mu);
        std::set<std::string> got;
        for (const auto& d : received) {
            if (d.size() == kRealSize) got.insert(std::string(d.begin(), d.end()));
        }
        CHECK_EQ(got.size(), static_cast<std::size_t>(kCount));  // 无重复
        CHECK(got == expected);
    }
    p.stop();
}

// 5. send_file 抗丢包：channel_reliable[2]，drop_every(33)（~3% 现网上限），
//    ~120KB 随机内容文件完整到达，文件名与全部字节一致。
TEST_CASE(e2e_send_file_survives_loss) {
    auto ca = e2e::make_config("A", 11041);
    auto cb = e2e::make_config("B", 11042);
    ca.channel_reliable[2] = true;
    cb.channel_reliable[2] = true;
    // 同 send_data 用例：关闭拥塞排空窗口，避免 clear_outbound 清掉
    // file 数据报（块级 NACK 重传语义才是本用例的验证对象）。
    ca.slowdown_window_ms = 0;
    cb.slowdown_window_ms = 0;

    Pair p(ca, cb);
    std::mutex file_mu;
    std::vector<std::pair<std::string, Bytes>> files;  // (name, data)
    p.b.set_file_callback([&](const std::string&, const std::string& name, Bytes data) {
        std::lock_guard<std::mutex> lk(file_mu);
        files.emplace_back(name, std::move(data));
    });
    p.start();
    CHECK(p.connect_ab());
    p.link.set_ab_policy(drop_policy(e2e::drop_every(33)));

    auto got_file = [&](const std::string& name) {
        std::lock_guard<std::mutex> lk(file_mu);
        for (const auto& f : files) {
            if (f.first == name) return true;
        }
        return false;
    };
    // 热身小文件建立序列基线
    bool base = false;
    for (int i = 0; i < 15 && !base; ++i) {
        p.a.send_file("B", "warmup.bin", Bytes(100, 0x55));
        base = e2e::wait_for([&] { return got_file("warmup.bin"); }, 300ms);
    }
    CHECK(base);

    std::mt19937 rng(42);
    Bytes content(120 * 1024);
    for (auto& byte : content) byte = static_cast<std::uint8_t>(rng() & 0xFF);
    CHECK(p.a.send_file("B", "payload-120k.bin", content));
    // 收尾小文件揭示尾部缺口（块级 NACK 重传完成前持续揭示）
    for (int i = 0; i < 60 && !got_file("payload-120k.bin"); ++i) {
        p.a.send_file("B", "trailer.bin", Bytes(16, 0x66));
        e2e::wait_for([&] { return got_file("payload-120k.bin"); }, 500ms);
    }
    CHECK(got_file("payload-120k.bin"));
    {
        std::lock_guard<std::mutex> lk(file_mu);
        for (const auto& f : files) {
            if (f.first == "payload-120k.bin") {
                CHECK_EQ(f.second.size(), content.size());
                CHECK(f.second == content);
            }
        }
    }
    p.stop();
}

// 6. 命令通道保序：A→B 100ms 固定延迟 + 20ms 抖动制造乱序，10 条命令按序交付。
TEST_CASE(e2e_command_channel_in_order_under_jitter) {
    auto ca = e2e::make_config("A", 11061);
    auto cb = e2e::make_config("B", 11062);

    Pair p(ca, cb);
    std::mutex cmd_mu;
    std::vector<int> seqs;
    bool warmup_done = false;
    p.b.set_command_callback([&](const std::string&, Bytes payload) {
        std::lock_guard<std::mutex> lk(cmd_mu);
        if (payload.size() == 2 && payload[0] == 0xC0) {
            seqs.push_back(static_cast<int>(payload[1]));
        } else if (payload.size() == 1 && payload[0] == 0xC1) {
            warmup_done = true;
        }
    });
    p.start();
    CHECK(p.connect_ab());
    e2e::ScriptedSocket::Policy pol;
    pol.delay = 100ms;
    pol.jitter = 20ms;
    p.link.set_ab_policy(pol);
    // 等 RTT 估计追平新链路延迟再发命令：接收端乱序保持窗口 = 3×RTT，
    // 策略在握手后才施加，此刻 RTT 估计仍是握手期的 ~0（窗口仅几 ms
    // ~30ms），20ms 抖动 + Windows 定时精度 ~15.6ms 会把乱序包判成
    // "超窗跳号"而丢弃（实测偶发）。报告每 100ms 喂一次单程延迟样本
    // （EWMA 1/8），1.5s 后估计收敛到 ~200ms（窗口 ~600ms），覆盖最坏
    // 迟到 ~140ms。
    std::this_thread::sleep_for(1500ms);
    // 热身命令（0xC1）：建立命令通道序号基线并验证通路
    CHECK(p.a.send_command("B", Bytes{0xC1}));
    CHECK(e2e::wait_for([&] {
        std::lock_guard<std::mutex> lk(cmd_mu);
        return warmup_done;
    }));

    for (int i = 0; i < 10; ++i) {
        Bytes payload{0xC0, static_cast<std::uint8_t>(i)};
        CHECK(p.a.send_command("B", std::move(payload)));
    }
    CHECK(e2e::wait_for([&] {
        std::lock_guard<std::mutex> lk(cmd_mu);
        return seqs.size() >= 10;
    }));
    {
        std::lock_guard<std::mutex> lk(cmd_mu);
        CHECK_EQ(seqs.size(), 10U);
        for (int i = 0; i < 10 && i < static_cast<int>(seqs.size()); ++i) {
            CHECK_EQ(seqs[i], i);
        }
    }
    p.stop();
}
