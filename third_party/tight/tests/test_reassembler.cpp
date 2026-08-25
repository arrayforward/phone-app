#include "test_framework.hpp"

#include "test_data_helpers.hpp"

#include "core/peer.hpp"
#include "channel/reassembler.hpp"
#include "protocol/wire_format.hpp"
#include "tight/fec.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

using namespace tight;
using namespace tight::tight_detail;
using namespace test_helpers;
using namespace std::chrono_literals;

namespace {

Bytes make_payload(std::size_t size, std::uint8_t seed) {
    Bytes p(size);
    for (std::size_t i = 0; i < size; ++i)
        p[i] = static_cast<std::uint8_t>(seed + i * 13);
    return p;
}

// 模拟 fragmenter 出站流：4 字节总长前缀 + 负载
Bytes make_stream(const Bytes& payload) {
    std::uint32_t total = static_cast<std::uint32_t>(payload.size());
    Bytes stream(4 + payload.size());
    std::uint32_t be = to_be32(total);
    std::memcpy(stream.data(), &be, 4);
    std::copy(payload.begin(), payload.end(), stream.begin() + 4);
    return stream;
}

} // namespace

TEST_CASE(reassembler_single_fragment_delivery) {
    Peer peer;
    auto stream = make_stream({'h', 'i'});
    std::vector<CapturedFragment> frags = {
        {1, 1, 0, 1, 1, static_cast<std::uint16_t>(stream.size()), 64, stream}};
    std::vector<Bytes> delivered;
    test_helpers::feed_fragments_to_reassembler(
        peer, frags, [&](Peer*, Bytes payload) { delivered.push_back(std::move(payload)); });
    CHECK_EQ(delivered.size(), 1U);
    CHECK(delivered[0] == Bytes({'h', 'i'}));
    // 重复投递同一消息被 m_completed 拦截
    test_helpers::feed_fragments_to_reassembler(
        peer, frags, [&](Peer*, Bytes payload) { delivered.push_back(std::move(payload)); });
    CHECK_EQ(delivered.size(), 1U);
}

TEST_CASE(reassembler_multi_fragment_delivery) {
    Peer peer;
    auto payload = make_payload(3000, 7);
    auto stream = make_stream(payload);
    std::vector<CapturedFragment> frags;
    std::size_t width = 1302;
    std::uint16_t cnt = static_cast<std::uint16_t>((stream.size() + width - 1) / width);
    for (std::size_t i = 0; i < stream.size(); i += width) {
        std::size_t len = std::min(width, stream.size() - i);
        frags.push_back({10, static_cast<std::uint32_t>(i / width + 1),  // seq 1..3
                         static_cast<std::uint16_t>(i / width), cnt, cnt,
                         static_cast<std::uint16_t>(len), static_cast<std::uint16_t>(width),
                         Bytes(stream.begin() + i, stream.begin() + i + len)});
    }
    CHECK_EQ(cnt, 3);
    // 乱序到达：frag 1 先到，0、2 后到
    std::vector<Bytes> delivered;
    test_helpers::feed_fragments_to_reassembler(
        peer, {frags[1]},
        [&](Peer*, Bytes p) { delivered.push_back(std::move(p)); });
    CHECK(delivered.empty()); // 缺口窗口内不投递
    test_helpers::feed_fragments_to_reassembler(
        peer, {frags[0], frags[2]},
        [&](Peer*, Bytes p) { delivered.push_back(std::move(p)); });
    CHECK_EQ(delivered.size(), 1U);
    CHECK(delivered[0] == payload);
}

TEST_CASE(reassembler_fec_recovers_missing_fragment) {
    Peer peer;
    auto payload = make_payload(2000, 3);
    auto stream = make_stream(payload);
    std::size_t width = 1302;
    std::vector<CapturedFragment> frags = {
        {20, 1, 0, 3, 2, 1302, static_cast<std::uint16_t>(width),
         Bytes(stream.begin(), stream.begin() + 1302)},
        {20, 2, 1, 3, 2, static_cast<std::uint16_t>(stream.size() - 1302),
         static_cast<std::uint16_t>(width),
         Bytes(stream.begin() + 1302, stream.end())},
    };
    // 1 片校验：数据片 0 丢失时用 parity 恢复
    std::vector<Bytes> data_frags = {frags[0].data, frags[1].data};
    auto parities = ReedSolomon::encode(data_frags, 1, width);
    CapturedFragment parity = {20, 0, 2, 3, 2, 1302,
                               static_cast<std::uint16_t>(width), parities[0]};

    std::vector<Bytes> delivered;
    std::vector<std::uint8_t> lost_channels;
    auto deliver = [&](Peer*, Bytes p) { delivered.push_back(std::move(p)); };
    auto on_loss = [&](Peer*, std::uint8_t ch) { lost_channels.push_back(ch); };

    // 只有校验片：缺 2 个数据片 > 1 片能力，窗口内等待
    test_helpers::feed_fragments_to_reassembler(peer, {parity}, deliver, on_loss);
    CHECK(delivered.empty());
    CHECK(lost_channels.empty());
    // 校验片 + 缺一个数据片：RS 即时恢复并投递
    test_helpers::feed_fragments_to_reassembler(peer, {frags[0], parity}, deliver, on_loss);
    CHECK_EQ(delivered.size(), 1U);
    CHECK(delivered[0] == payload);
    CHECK(lost_channels.empty());
}

TEST_CASE(reassembler_loss_after_wait_window) {
    Peer peer;
    auto payload = make_payload(2000, 9);
    auto stream = make_stream(payload);
    auto frag0 = CapturedFragment{30, 1, 0, 2, 2, 1302, 1302,
                                  Bytes(stream.begin(), stream.begin() + 1302)};

    std::vector<Bytes> delivered;
    std::vector<std::uint8_t> lost_channels;
    auto deliver = [&](Peer*, Bytes p) { delivered.push_back(std::move(p)); };
    auto on_loss = [&](Peer*, std::uint8_t ch) { lost_channels.push_back(ch); };
    test_helpers::feed_fragments_to_reassembler(peer, {frag0}, deliver, on_loss);
    CHECK(delivered.empty());
    CHECK(lost_channels.empty()); // 窗口内不误报
    std::this_thread::sleep_for(300ms); // 不可靠通道窗口 max(250ms, 2RTT)
    test_helpers::feed_fragments_to_reassembler(peer, {frag0}, deliver, on_loss);
    CHECK(delivered.empty());
    CHECK_EQ(lost_channels.size(), 1U);
    CHECK_EQ(lost_channels[0], 0U);
}

TEST_CASE(reassembler_rejects_oversize_fragment_count) {
    Peer peer;
    std::vector<std::uint8_t> lost;
    auto h = test_helpers::make_data_header(40, 1, 0, 2000, 2000, 64);
    bool delivered = false;
    Reassembler::handle_data(peer, h, Bytes(64, 0), 10000, 4.0, 0, 64 * 1024,
                             std::chrono::milliseconds(1000),
                             [&](Peer*, Bytes) { delivered = true; },
                             [&](Peer*, std::uint8_t) { lost.push_back(0); });
    CHECK(!delivered);
    CHECK(lost.empty());
}

TEST_CASE(reassembler_sequence_gap_tracking) {
    Peer peer;
    auto stream = make_stream({'a'});
    auto f1 = CapturedFragment{50, 1, 0, 1, 1, 4, 64, stream};
    auto f3 = CapturedFragment{51, 3, 0, 1, 1, 4, 64, stream};
    std::vector<Bytes> delivered;
    test_helpers::feed_fragments_to_reassembler(
        peer, {f1, f3},
        [&](Peer*, Bytes p) { delivered.push_back(std::move(p)); });
    CHECK_EQ(delivered.size(), 2U);
    CHECK_EQ(peer.m_next_expected_seq, 2U);
    CHECK_EQ(peer.m_missing_seqs.count(2), 1U);
    // 迟到补片 seq=2 到达：游标消化 2、3 前进到 4；缺口条目由 report
    // 周期统一清理（可靠通道重传晚到走 else 分支时即时移除）。
    auto f2 = CapturedFragment{52, 2, 0, 1, 1, 4, 64, stream};
    test_helpers::feed_fragments_to_reassembler(
        peer, {f2},
        [&](Peer*, Bytes p) { delivered.push_back(std::move(p)); });
    CHECK_EQ(delivered.size(), 3U);
    CHECK_EQ(peer.m_next_expected_seq, 4U);
    // 晚到重传（游标已越过 seq=2）走 else 分支：即时从缺口表移除
    auto f2_retx = CapturedFragment{53, 2, 0, 1, 1, 4, 64, stream};
    test_helpers::feed_fragments_to_reassembler(
        peer, {f2_retx},
        [&](Peer*, Bytes p) { delivered.push_back(std::move(p)); });
    CHECK_EQ(peer.m_missing_seqs.count(2), 0U);
}

TEST_CASE(reassembler_parity_seq_zero_does_not_init_base) {
    Peer peer;
    // Parity 报文（seq=0）先到，不得初始化序列基准
    auto h = test_helpers::make_data_header(60, 0, 2, 2, 1, 64);
    Reassembler::handle_data(peer, h, Bytes(64, 0), 10000, 4.0, 0, 64 * 1024,
                             std::chrono::milliseconds(1000),
                             [&](Peer*, Bytes) {});
    CHECK(!peer.m_seq_initialized);
    // 随后数据报文 seq=1 正常初始化
    auto stream = make_stream({'x'});
    auto f = CapturedFragment{61, 1, 0, 1, 1, 4, 64, stream};
    test_helpers::feed_fragments_to_reassembler(
        peer, {f}, [&](Peer*, Bytes) {});
    CHECK(peer.m_seq_initialized);
    CHECK_EQ(peer.m_next_expected_seq, 2U);
}

TEST_CASE(reassembler_channel_parsed_from_reserved) {
    auto h = test_helpers::make_data_header(70, 1, 0, 1, 1, 4, 5);
    CHECK_EQ(channel_of(h.reserved), 5U);
    auto h2 = test_helpers::make_data_header(71, 1, 0, 1, 1, 4, 0);
    CHECK_EQ(channel_of(h2.reserved), 0U);
    // reserved 低 12 位保留 real_size
    CHECK_EQ(h2.reserved & kRealSizeMask, 4);
}

// 关键帧报文（线上 reserved bit11）不进报文级延迟统计：大 IDR 突发的
// 串行化排队是帧自身传输时间，不应污染 P50 直方图与迟到样本。
TEST_CASE(reassembler_keyframe_packets_skip_latency_stats) {
    Peer peer;
    peer.m_clock_synced = true;
    peer.m_clock_offset_us = 0;
    auto stream = make_stream({'a', 'b'});
    Bytes wire(64, 0);
    std::copy(stream.begin(), stream.end(), wire.begin());
    auto deliver = [](Peer*, Bytes) {};

    // 关键帧分片：transit/直方图样本均为 0
    auto kh = make_data_header(1, 1, 0, 1, 1,
                               static_cast<std::uint16_t>(stream.size()));
    kh.reserved |= kKeyframeMask;
    Reassembler::handle_data(peer, kh, wire, 10000, 4.0, 16, 64 * 1024,
                             std::chrono::milliseconds(1000), deliver, {});
    CHECK_EQ(peer.m_transit_samples, 0U);
    CHECK_EQ(peer.m_hist_samples, 0U);

    // 普通分片：照常统计
    auto nh = make_data_header(2, 2, 0, 1, 1,
                               static_cast<std::uint16_t>(stream.size()));
    Reassembler::handle_data(peer, nh, wire, 10000, 4.0, 16, 64 * 1024,
                             std::chrono::milliseconds(1000), deliver, {});
    CHECK_EQ(peer.m_transit_samples, 1U);
    CHECK_EQ(peer.m_hist_samples, 1U);
}

// is_keyframe_packet 与 channel_of 在 bit11 共存下的正确性
TEST_CASE(reassembler_keyframe_bit_coexists_with_channel) {
    PacketHeader h;
    h.reserved = static_cast<std::uint16_t>((3 << kChannelShift) | kKeyframeMask | 100);
    CHECK(is_keyframe_packet(h.reserved));
    CHECK_EQ(channel_of(h.reserved), 3U);
    CHECK_EQ(h.reserved & kRealSizeMask, 100U);
    PacketHeader h2;
    h2.reserved = static_cast<std::uint16_t>((5 << kChannelShift) | 100);
    CHECK(!is_keyframe_packet(h2.reserved));
    CHECK_EQ(channel_of(h2.reserved), 5U);
}
