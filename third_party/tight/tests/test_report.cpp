// Unit tests for channel/report: periodic ACK/NACK report building
// (Report::build_payload) and report processing (Report::handle).

#include "test_framework.hpp"

#include "channel/report.hpp"
#include "core/peer.hpp"
#include "protocol/wire_format.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

using namespace tight;
using namespace tight::tight_detail;
using namespace std::chrono_literals;

namespace {

// ---- wire payload readers (layout documented in channel/report.hpp) ----

std::uint32_t rd32(const Bytes& p, std::size_t off) {
    std::uint32_t v = 0;
    std::memcpy(&v, p.data() + off, 4);
    return to_be32(v);
}

std::uint16_t rd16(const Bytes& p, std::size_t off) {
    std::uint16_t v = 0;
    std::memcpy(&v, p.data() + off, 2);
    return to_be16(v);
}

// ---- peer / pending construction helpers ----

PendingSend make_pending(std::uint32_t seq, std::uint32_t retries = 0) {
    PendingSend ps;
    ps.m_header.sequence = seq;
    ps.m_header.payload_size = 100;
    ps.m_payload = Bytes(100, static_cast<std::uint8_t>(seq & 0xFF));
    ps.m_bytes = 48 + ps.m_payload.size();
    ps.m_last_send = std::chrono::steady_clock::now();
    ps.m_retries = retries;
    return ps;
}

// Receiver peer with sequence tracking initialized on reliable channel 0.
void init_receiver(Peer& peer, std::uint32_t next_expected) {
    peer.m_seq_initialized = true;
    peer.m_next_expected_seq = next_expected;
    peer.m_channel_reliable[0] = true;
    peer.m_peer_retransmit = true;
}

void add_missing(Peer& peer, std::uint32_t seq, std::chrono::milliseconds age,
                 std::uint8_t channel = 0) {
    peer.m_missing_seqs[seq] = std::chrono::steady_clock::now() - age;
    peer.m_missing_channel[seq] = channel;
}

struct ResendCapture {
    std::vector<std::pair<std::uint32_t, Bytes>> calls;
    Report::ResendCallback callback() {
        return [this](Peer*, const PacketHeader& h, const Bytes& payload) {
            calls.emplace_back(h.sequence, payload);
        };
    }
};

Report::ResendCallback null_resend() {
    return [](Peer*, const PacketHeader&, const Bytes&) {};
}

} // namespace

// 空报告往返：接收侧无缺口无流量，发送侧无 pending——不崩溃、无重传、
// 计数清零、报告标志置位。
TEST_CASE(report_empty_roundtrip) {
    Peer receiver;
    init_receiver(receiver, 11);
    Bytes payload = Report::build_payload(receiver, 333ms, 0, true);
    CHECK_GE(payload.size(), 12U);
    CHECK_EQ(rd32(payload, 0), 10U);  // ack cursor = next_expected - 1
    CHECK_EQ(rd16(payload, 4), 0);    // late ratio
    CHECK_EQ(rd16(payload, 6), 0);    // lost count

    Peer sender;
    ResendCapture cap;
    ReportResult r = Report::handle(sender, payload, cap.callback());
    CHECK(cap.calls.empty());
    CHECK(sender.m_pending.empty());
    CHECK_EQ(r.probe_bw, 0U);
    CHECK_EQ(r.recv_rate, 0U);
    CHECK_EQ(r.loss_ratio, 0);
    CHECK_EQ(r.ce_ratio, 0);
    CHECK(sender.m_have_late_report);
    CHECK_EQ(sender.m_peer_late_ratio, 0.0);
}

// ack 游标推进后，发送端 m_pending 中 <= ack 的序号被剪枝，> ack 的保留。
TEST_CASE(report_ack_cursor_prunes_pending) {
    Peer receiver;  // 已连续收到 1..5
    init_receiver(receiver, 6);
    Bytes payload = Report::build_payload(receiver, 333ms, 0, true);
    CHECK_EQ(rd32(payload, 0), 5U);

    Peer sender;
    for (std::uint32_t s = 3; s <= 7; ++s) sender.m_pending[s] = make_pending(s);
    ResendCapture cap;
    Report::handle(sender, payload, cap.callback());
    CHECK(cap.calls.empty());
    CHECK_EQ(sender.m_pending.size(), 2U);
    CHECK_EQ(sender.m_pending.count(3), 0U);
    CHECK_EQ(sender.m_pending.count(4), 0U);
    CHECK_EQ(sender.m_pending.count(5), 0U);
    CHECK_EQ(sender.m_pending.count(6), 1U);
    CHECK_EQ(sender.m_pending.count(7), 1U);
}

// 缺口生成 NACK：接收侧缺 seq 1（已收到 2、3），报告携带丢失序号；
// 发送端对缺失序号触发重传回调，并修剪已确认的 pending。
TEST_CASE(report_gap_generates_nack_and_resend) {
    Peer receiver;
    init_receiver(receiver, 1);
    receiver.m_recv_seqs = {2, 3};
    add_missing(receiver, 1, 100ms);  // 超过 3.5xRTT 阈值（默认 35ms）
    Bytes payload = Report::build_payload(receiver, 333ms, 0, true);

    // 线上字段：ack 跳过缺口并消化 2、3 → ack=3；NACK 列表含 seq 1
    CHECK_EQ(rd32(payload, 0), 3U);
    CHECK_EQ(rd16(payload, 6), 1U);
    CHECK_EQ(rd32(payload, 12), 1U);
    // 丢包率 = 跳过 1 / 游标推进 3 = 3333 (x10000)；N=1 时尾部后移 4 字节
    CHECK_EQ(rd16(payload, 24), 3333);
    // 缺口仍在跟踪（重传到达前每周期重复上报）
    CHECK_EQ(receiver.m_missing_seqs.count(1), 1U);

    Peer sender;
    for (std::uint32_t s = 1; s <= 4; ++s) sender.m_pending[s] = make_pending(s);
    ResendCapture cap;
    ReportResult r = Report::handle(sender, payload, cap.callback());

    // seq 1 触发重传，(header, payload) 原样回调
    CHECK_EQ(cap.calls.size(), 1U);
    CHECK_EQ(cap.calls[0].first, 1U);
    CHECK(cap.calls[0].second == Bytes(100, 1));
    // 已确认的 2、3 被修剪；1 保留且重传计数 +1；4 未确认保留
    CHECK_EQ(sender.m_pending.size(), 2U);
    CHECK_EQ(sender.m_pending.count(2), 0U);
    CHECK_EQ(sender.m_pending.count(3), 0U);
    CHECK_EQ(sender.m_pending.at(1).m_retries, 1U);
    CHECK_EQ(sender.m_pending.count(4), 1U);
    CHECK_EQ(r.loss_ratio, 3333);
}

// 未达丢失阈值的新鲜缺口不上报 NACK、不跳过，ack 游标停滞。
TEST_CASE(report_fresh_gap_below_threshold_no_nack) {
    Peer receiver;
    init_receiver(receiver, 1);
    receiver.m_recv_seqs = {2, 3};
    add_missing(receiver, 1, 1ms);  // 远小于 35ms 阈值
    Bytes payload = Report::build_payload(receiver, 333ms, 0, true);
    CHECK_EQ(rd16(payload, 6), 0U);   // 无 NACK
    CHECK_EQ(rd32(payload, 0), 0U);   // 游标未推进（next_expected=1 → ack=0）
    CHECK_EQ(receiver.m_missing_seqs.count(1), 1U);
}

// 不可靠通道（或对端未通告重传）的缺口立即跳过、不上报 NACK。
TEST_CASE(report_unreliable_channel_gap_skipped) {
    Peer receiver;
    init_receiver(receiver, 1);
    receiver.m_recv_seqs = {2};
    receiver.m_channel_reliable[0] = false;  // 纯 FEC 通道
    add_missing(receiver, 1, 100ms);
    Bytes payload = Report::build_payload(receiver, 333ms, 0, true);
    CHECK_EQ(rd16(payload, 6), 0U);   // 无 NACK
    CHECK_EQ(rd32(payload, 0), 2U);   // 游标跳过缺口并消化 2
    CHECK(receiver.m_missing_seqs.empty());
    CHECK(receiver.m_missing_channel.empty());

    // 对端未通告重传：同样跳过不上报
    Peer receiver2;
    init_receiver(receiver2, 1);
    receiver2.m_peer_retransmit = false;
    add_missing(receiver2, 1, 100ms);
    Bytes payload2 = Report::build_payload(receiver2, 333ms, 0, true);
    CHECK_EQ(rd16(payload2, 6), 0U);
    CHECK(receiver2.m_missing_seqs.empty());
}

// 超过 (kMaxRetries+2) 个周期未收到重传：放弃上报，缺口从跟踪表擦除。
TEST_CASE(report_give_up_stops_nack) {
    Peer receiver;
    init_receiver(receiver, 5);
    add_missing(receiver, 5, 3s);  // give_up = 100ms * 12 = 1.2s
    Bytes payload = Report::build_payload(receiver, 100ms, 0, true);
    CHECK_EQ(rd16(payload, 6), 0U);
    CHECK(receiver.m_missing_seqs.empty());
    CHECK(receiver.m_missing_channel.empty());
}

// NACK 列表硬上限 256：300 个缺口只上报前 256 个。
TEST_CASE(report_nack_list_capped_at_256) {
    Peer receiver;
    init_receiver(receiver, 1);
    for (std::uint32_t s = 1; s <= 300; ++s) add_missing(receiver, s, 100ms);
    Bytes payload = Report::build_payload(receiver, 333ms, 0, true);
    CHECK_EQ(rd16(payload, 6), 256U);
    CHECK_EQ(rd32(payload, 12), 1U);            // 首个丢失序号
    CHECK_EQ(rd32(payload, 12 + 255 * 4), 256U); // 第 256 个
    // 游标连续跳过 300 个缺口
    CHECK_EQ(rd32(payload, 0), 300U);
}

// 统计字段往返：迟到率（非直方图路径）、接收速率、CE 占比、探测带宽。
TEST_CASE(report_stats_fields_roundtrip) {
    Peer receiver;
    init_receiver(receiver, 1);
    receiver.m_transit_samples = 10;
    receiver.m_late_samples = 2;    // late_buffer_ms=0 → 迟到率 2/10 = 0.2
    receiver.m_recv_bytes = 8192;   // ≥4096 门槛 → recv_rate = 8192 B/s (1s 间隔)
    receiver.m_ce_marks = 5;
    receiver.m_data_pkts = 45;      // total 50 ≥ 20 → ce_ratio = 1000
    receiver.m_probe_bw_bps = 123456;
    Bytes payload = Report::build_payload(receiver, 1000ms, 0, true);
    CHECK_EQ(rd16(payload, 4), 2000);          // late ratio x10000
    CHECK_EQ(rd32(payload, 12), 123456U);      // probe_bw
    CHECK_EQ(rd32(payload, 16), 8192U);        // recv_rate
    CHECK_EQ(rd16(payload, 22), 1000);         // ce_ratio

    // build 后 per-interval 计数清零、probe 带宽只随报告携带一次
    CHECK_EQ(receiver.m_transit_samples, 0U);
    CHECK_EQ(receiver.m_late_samples, 0U);
    CHECK_EQ(receiver.m_recv_bytes, 0U);
    CHECK_EQ(receiver.m_ce_marks, 0U);
    CHECK_EQ(receiver.m_data_pkts, 0U);
    CHECK_EQ(receiver.m_probe_bw_bps, 0U);
    Bytes payload2 = Report::build_payload(receiver, 1000ms, 0, true);
    CHECK_EQ(rd32(payload2, 12), 0U);

    Peer sender;
    ReportResult r = Report::handle(sender, payload, null_resend());
    CHECK_EQ(r.probe_bw, 123456U);
    CHECK_EQ(r.recv_rate, 8192U);
    CHECK_EQ(r.ce_ratio, 1000);
    CHECK_EQ(r.loss_ratio, 0);
    CHECK_EQ(sender.m_peer_late_ratio, 0.2);
    CHECK(sender.m_have_late_report);
}

// 直方图路径：P50 中位数写入报告尾部，handle 侧存入 m_peer_p50_ms。
TEST_CASE(report_latency_histogram_p50_roundtrip) {
    Peer receiver;
    init_receiver(receiver, 1);
    // 10 个样本全在 bin 1（8~16ms）→ P50 = 12ms
    receiver.m_latency_hist[1] = 10;
    receiver.m_hist_samples = 10;
    Bytes payload = Report::build_payload(receiver, 333ms, 8, true);
    CHECK_EQ(rd16(payload, 24), 12);  // p50_ms 字段
    // 直方图已清零，迟到线 = P50 + late_buffer = 20ms
    CHECK_EQ(receiver.m_hist_samples, 0U);
    CHECK_EQ(receiver.m_late_line_us, 20000U);

    Peer sender;
    Report::handle(sender, payload, null_resend());
    CHECK_EQ(sender.m_peer_p50_ms, 12);
}

// 空流量间隔：接收字节低于 4096 门槛时 recv_rate 上报 0。
TEST_CASE(report_low_traffic_recv_rate_zero) {
    Peer receiver;
    init_receiver(receiver, 1);
    receiver.m_recv_bytes = 91;
    Bytes payload = Report::build_payload(receiver, 333ms, 0, true);
    CHECK_EQ(rd32(payload, 16), 0U);
    CHECK_EQ(receiver.m_recv_bytes, 0U);
}

// 探测列车 finalize：间隔超 20ms 无新 probe → 测速完成，带宽随报告携带。
TEST_CASE(report_probe_train_finalize) {
    Peer receiver;
    init_receiver(receiver, 1);
    auto now = std::chrono::steady_clock::now();
    receiver.m_probe_first = now - 100ms;
    receiver.m_probe_last = now - 50ms;   // gap > kProbeTrainGap(20ms)
    receiver.m_probe_bytes = 3000;
    receiver.m_probe_count = 3;
    Bytes payload = Report::build_payload(receiver, 333ms, 0, true);
    // bw = 3000 B / 50ms = 60000 B/s
    CHECK_EQ(rd32(payload, 12), 60000U);
    CHECK_EQ(receiver.m_probe_count, 0U);
    CHECK_EQ(receiver.m_probe_bytes, 0U);

    Peer sender;
    ReportResult r = Report::handle(sender, payload, null_resend());
    CHECK_EQ(r.probe_bw, 60000U);
}

// 列车仍在到达（gap < 20ms）：不 finalize，报告不携带带宽，计数保留。
TEST_CASE(report_probe_train_in_flight_not_finalized) {
    Peer receiver;
    init_receiver(receiver, 1);
    auto now = std::chrono::steady_clock::now();
    receiver.m_probe_first = now - 10ms;
    receiver.m_probe_last = now;  // 刚到，gap 未超 20ms
    receiver.m_probe_bytes = 1500;
    receiver.m_probe_count = 3;
    Bytes payload = Report::build_payload(receiver, 333ms, 0, true);
    CHECK_EQ(rd32(payload, 12), 0U);
    CHECK_EQ(receiver.m_probe_count, 3U);
    CHECK_EQ(receiver.m_probe_bytes, 1500U);
}

// 帧级迟到统计 lite 模式：F_max + 64bin 直方图往返，计数 clamp 255。
TEST_CASE(report_frame_stats_lite_roundtrip) {
    Peer receiver;
    init_receiver(receiver, 1);
    receiver.m_frame_max_bytes = 5000;
    receiver.m_frame_latency_hist[2] = 3;
    receiver.m_frame_latency_hist[10] = 1;
    receiver.m_frame_latency_hist[63] = 300;  // clamp 到 255
    receiver.m_frame_hist_samples = 304;
    Bytes payload = Report::build_payload(receiver, 333ms, 0, /*lite_mode=*/true);

    // 帧统计尾部：base = 12 + 0*4 + 14 = 26
    CHECK_EQ(payload.size(), 26U + 3 + 64);
    CHECK_EQ(payload[26], 0);  // flags bit0 = 0 → lite

    // build 后帧统计清零
    CHECK_EQ(receiver.m_frame_max_bytes, 0U);
    CHECK_EQ(receiver.m_frame_hist_samples, 0U);

    Peer sender;
    ReportResult r = Report::handle(sender, payload, null_resend());
    CHECK(r.m_frame_lite);
    CHECK_EQ(r.m_frame_max_bytes, 5000U);
    CHECK_EQ(r.m_frame_hist[2], 3U);
    CHECK_EQ(r.m_frame_hist[10], 1U);
    CHECK_EQ(r.m_frame_hist[63], 255U);
    CHECK_EQ(r.m_frame_hist_samples, 259U);  // 3 + 1 + 255
    CHECK(r.m_frame_pairs.empty());
}

// 帧级迟到统计正常模式：逐帧 (F, D) 对往返。
TEST_CASE(report_frame_stats_normal_roundtrip) {
    Peer receiver;
    init_receiver(receiver, 1);
    receiver.m_frame_max_bytes = 2000;
    receiver.m_frame_pairs = {{1000, 5}, {2000, 12}};
    Bytes payload = Report::build_payload(receiver, 333ms, 0, /*lite_mode=*/false);
    CHECK_EQ(payload[26], 1);  // flags bit0 = 1 → normal
    CHECK(receiver.m_frame_pairs.empty());

    Peer sender;
    ReportResult r = Report::handle(sender, payload, null_resend());
    CHECK(!r.m_frame_lite);
    CHECK_EQ(r.m_frame_max_bytes, 2000U);
    CHECK_EQ(r.m_frame_pairs.size(), 2U);
    CHECK_EQ(r.m_frame_pairs[0].first, 1000);
    CHECK_EQ(r.m_frame_pairs[0].second, 5);
    CHECK_EQ(r.m_frame_pairs[1].first, 2000);
    CHECK_EQ(r.m_frame_pairs[1].second, 12);
}

// 重复报告幂等：同一 ack 游标的纯 ACK 报告处理两次——第二次不重复
// 修剪、不触发重传、不崩溃。
TEST_CASE(report_duplicate_ack_report_idempotent) {
    Peer receiver;
    init_receiver(receiver, 6);
    Bytes payload = Report::build_payload(receiver, 333ms, 0, true);

    Peer sender;
    for (std::uint32_t s = 4; s <= 7; ++s) sender.m_pending[s] = make_pending(s);
    ResendCapture cap;
    Report::handle(sender, payload, cap.callback());
    CHECK_EQ(sender.m_pending.size(), 2U);
    Report::handle(sender, payload, cap.callback());
    CHECK_EQ(sender.m_pending.size(), 2U);
    CHECK_EQ(sender.m_pending.count(6), 1U);
    CHECK_EQ(sender.m_pending.count(7), 1U);
    CHECK(cap.calls.empty());
    // 过期（更小）ack 游标同样无副作用
    Bytes stale(12, 0);
    std::uint32_t ack_be = to_be32(2);
    std::memcpy(stale.data(), &ack_be, 4);
    Report::handle(sender, stale, cap.callback());
    CHECK_EQ(sender.m_pending.size(), 2U);
    CHECK(cap.calls.empty());
}

// 重传次数耗尽的 pending 一并修剪：NACK 不再触发重传。
TEST_CASE(report_retries_exhausted_pending_pruned) {
    // 手工构造报告：ack=4，lost={5}
    Bytes payload(16, 0);
    std::uint32_t ack_be = to_be32(4);
    std::uint16_t cnt_be = to_be16(1);
    std::uint32_t lost_be = to_be32(5);
    std::memcpy(payload.data(), &ack_be, 4);
    std::memcpy(payload.data() + 6, &cnt_be, 2);
    std::memcpy(payload.data() + 12, &lost_be, 4);

    Peer sender;
    sender.m_pending[5] = make_pending(5, /*retries=*/10);  // kMaxRetries
    sender.m_pending[6] = make_pending(6);
    ResendCapture cap;
    Report::handle(sender, payload, cap.callback());
    CHECK(cap.calls.empty());               // 不再重传
    CHECK_EQ(sender.m_pending.count(5), 0U); // 耗尽修剪
    CHECK_EQ(sender.m_pending.count(6), 1U);
}

// NACK 的序号不在 pending 中（已确认/从未发送）：不重传、不崩溃。
TEST_CASE(report_nack_for_unknown_seq_no_resend) {
    Peer receiver;
    init_receiver(receiver, 1);
    receiver.m_recv_seqs = {2};
    add_missing(receiver, 1, 100ms);
    Bytes payload = Report::build_payload(receiver, 333ms, 0, true);
    CHECK_EQ(rd16(payload, 6), 1U);

    Peer sender;  // pending 为空
    ResendCapture cap;
    Report::handle(sender, payload, cap.callback());
    CHECK(cap.calls.empty());
    CHECK(sender.m_pending.empty());
}

// 畸形报文防御：过短、lost_count 与长度不符、截断尾部——均返回默认
// 结果或忽略缺失字段，不崩溃。
TEST_CASE(report_malformed_payloads_ignored) {
    Peer sender;
    sender.m_pending[1] = make_pending(1);
    ResendCapture cap;

    // 不足 12 字节头
    ReportResult r = Report::handle(sender, Bytes(8, 0), cap.callback());
    CHECK_EQ(r.probe_bw, 0U);
    CHECK_EQ(sender.m_pending.size(), 1U);

    // lost_count=2 但没有丢失列表
    Bytes bad(12, 0);
    std::uint16_t cnt_be = to_be16(2);
    std::memcpy(bad.data() + 6, &cnt_be, 2);
    r = Report::handle(sender, bad, cap.callback());
    CHECK_EQ(r.probe_bw, 0U);
    CHECK_EQ(sender.m_pending.size(), 1U);

    // 仅 12 字节头（无任何可选尾部字段）：合法，字段全为默认
    Bytes minimal(12, 0);
    std::uint32_t ack_be = to_be32(0);
    std::memcpy(minimal.data(), &ack_be, 4);
    r = Report::handle(sender, minimal, cap.callback());
    CHECK_EQ(r.probe_bw, 0U);
    CHECK_EQ(r.recv_rate, 0U);
    CHECK_EQ(r.loss_ratio, 0);
    CHECK_EQ(r.ce_ratio, 0);
    CHECK_EQ(r.m_frame_hist_samples, 0U);
    CHECK_EQ(sender.m_pending.size(), 1U);

    // 头部 + 截断的可选尾部（只有 2 字节 probe_bw）：忽略尾部
    Bytes trunc(14, 0);
    r = Report::handle(sender, trunc, cap.callback());
    CHECK_EQ(r.probe_bw, 0U);

    CHECK(cap.calls.empty());
}
