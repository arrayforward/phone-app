#include "test_framework.hpp"

#include "test_data_helpers.hpp"

#include "channel/fragmenter.hpp"
#include "core/peer.hpp"
#include "protocol/wire_format.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace tight;
using namespace tight::tight_detail;
using namespace test_helpers;

namespace {

Bytes make_payload(std::size_t size, std::uint8_t seed) {
    Bytes p(size);
    for (std::size_t i = 0; i < size; ++i)
        p[i] = static_cast<std::uint8_t>(seed + i * 11);
    return p;
}

std::vector<CapturedFragment> run_fragmenter(
    Peer& peer, Bytes payload, std::size_t mtu, std::uint8_t channel = 0,
    std::uint16_t channel_fec_extra = 0, std::uint16_t probe_extra_parity = 0) {
    std::vector<CapturedFragment> out;
    std::uint32_t seq_out = 1;
    Fragmenter::fragment_and_send(
        peer, std::move(payload), mtu,
        [&](Peer*, std::uint32_t msg_id, std::uint16_t idx, std::uint16_t cnt,
            std::uint16_t data_cnt, std::uint16_t real_size,
            const std::uint8_t* frag_data, std::size_t frag_len,
            std::size_t width, bool ackable, bool keyframe) {
            (void)keyframe;
            CapturedFragment f;
            f.msg_id = msg_id;
            f.seq = (idx < data_cnt) ? seq_out++ : 0;
            f.idx = idx;
            f.cnt = cnt;
            f.data_cnt = data_cnt;
            f.real_size = real_size;
            f.width = static_cast<std::uint16_t>(width);
            f.data.assign(frag_data, frag_data + frag_len);
            out.push_back(std::move(f));
        },
        channel, channel_fec_extra, probe_extra_parity);
    return out;
}

} // namespace

TEST_CASE(fragmenter_parity_count_stage_0) {
    CHECK_EQ(Fragmenter::compute_parity_count_for(0.05, 10, 0), 0U);
    CHECK_EQ(Fragmenter::compute_parity_count_for(0.5, 10, 0), 0U);
}

TEST_CASE(fragmenter_parity_count_stage_1) {
    CHECK_EQ(Fragmenter::compute_parity_count_for(0.005, 10, 1), 1U);
    CHECK_EQ(Fragmenter::compute_parity_count_for(0.9, 10, 1), 1U);
}

TEST_CASE(fragmenter_parity_count_stage_2_entropy) {
    // p=0.01：H=0.0808 ×1.2=0.097 → 10 片数据 → ceil(0.97)=1（< 20% 上限）
    CHECK_EQ(Fragmenter::compute_parity_count_for(0.01, 10, 2), 1U);
    // p=0.3：H=0.8813 ×1.2=1.058 → 10 片 → 11，受 20% 冗余上限钳制 → ceil(10×0.2)=2
    CHECK_EQ(Fragmenter::compute_parity_count_for(0.3, 10, 2), 2U);
    // p=0.9：H=0.469 ×1.2=0.563，max(0.563,0.9)=0.9 → 10 片 → 9，同样钳制到 2
    CHECK_EQ(Fragmenter::compute_parity_count_for(0.9, 10, 2), 2U);
    // 极端值安全阀门
    CHECK_EQ(Fragmenter::compute_parity_count_for(0.0001, 10, 2), 1U);
    CHECK_EQ(Fragmenter::compute_parity_count_for(0.9999, 10, 2), 1U);
    // 大数据量：20% 上限 20000，再受 100 安全阀门
    CHECK_EQ(Fragmenter::compute_parity_count_for(0.5, 100000, 2), 100U);
    // 上限最小保护：单分片消息至少 1 片
    CHECK_EQ(Fragmenter::compute_parity_count_for(0.5, 1, 2), 1U);
}

TEST_CASE(fragmenter_single_fragment_no_report_starts_with_parity) {
    Peer peer;
    auto frags = run_fragmenter(peer, {'a', 'b'}, 1350);
    // 未收到对端 report：起步 2 片校验（单片消息封顶 1 片）
    CHECK_EQ(frags.size(), 2U);
    CHECK_EQ(frags[0].idx, 0U);
    CHECK_EQ(frags[0].cnt, 2U);
    CHECK_EQ(frags[0].data_cnt, 1U);
    CHECK_EQ(frags[0].real_size, 6U); // 4B 总长前缀 + 2B 负载
    CHECK_EQ(frags[0].width, 1302U);
    CHECK_EQ(frags[1].idx, 1U);
    CHECK_EQ(frags[1].seq, 0U); // 校验片 seq=0
    CHECK_EQ(frags[1].data.size(), 1302U);
}

TEST_CASE(fragmenter_with_report_stage1_single_parity) {
    Peer peer;
    peer.m_have_late_report = true;
    peer.m_peer_late_ratio = 0.005;
    peer.m_fec_stage = 1;
    auto frags = run_fragmenter(peer, {'x'}, 1350);
    CHECK_EQ(frags.size(), 2U); // 1 数据 + 1 校验
    CHECK_EQ(frags[0].data_cnt, 1U);
    CHECK_EQ(frags[1].idx, 1U);
}

TEST_CASE(fragmenter_stage_machine_hysteresis) {
    Peer peer;
    peer.m_have_late_report = true;
    // 起始 stage 0，late_ratio 0.001 → 停留 0 档
    peer.m_peer_late_ratio = 0.001;
    auto frags = run_fragmenter(peer, {'x'}, 1350);
    CHECK_EQ(peer.m_fec_stage, 0U);
    CHECK_EQ(frags.size(), 1U);
    // 0.3% 以上 → 升档 1
    peer.m_peer_late_ratio = 0.005;
    frags = run_fragmenter(peer, {'x'}, 1350);
    CHECK_EQ(peer.m_fec_stage, 1U);
    // 档 1 回落到 0 → 归零
    peer.m_peer_late_ratio = 0.0;
    frags = run_fragmenter(peer, {'x'}, 1350);
    CHECK_EQ(peer.m_fec_stage, 0U);
}

TEST_CASE(fragmenter_multi_fragment_and_reassembly_roundtrip) {
    Peer peer;
    peer.m_have_late_report = true;
    peer.m_peer_late_ratio = 0.005;
    peer.m_fec_stage = 1;
    auto payload = make_payload(3000, 5);
    auto frags = run_fragmenter(peer, payload, 1350);
    // 3004 字节 → 3 数据片 + 1 校验片
    CHECK_EQ(frags.size(), 4U);
    std::size_t data_cnt = 0;
    for (auto& f : frags)
        if (f.idx < f.data_cnt) ++data_cnt;
    CHECK_EQ(data_cnt, 3U);

    std::vector<Bytes> delivered;
    test_helpers::feed_fragments_to_reassembler(
        peer, frags, [&](Peer*, Bytes p) { delivered.push_back(std::move(p)); });
    CHECK_EQ(delivered.size(), 1U);
    CHECK(delivered[0] == payload);
}

TEST_CASE(fragmenter_fec_recovery_roundtrip) {
    Peer peer;
    peer.m_have_late_report = true;
    peer.m_peer_late_ratio = 0.005;
    peer.m_fec_stage = 1;
    auto payload = make_payload(3000, 6);
    auto frags = run_fragmenter(peer, payload, 1350);
    CHECK_EQ(frags.size(), 4U);

    // 丢掉 1 个数据片（idx=1），FEC 恢复后仍能完整投递
    std::vector<CapturedFragment> partial;
    for (auto& f : frags)
        if (f.idx != 1) partial.push_back(f);
    std::vector<Bytes> delivered;
    test_helpers::feed_fragments_to_reassembler(
        peer, partial, [&](Peer*, Bytes p) { delivered.push_back(std::move(p)); });
    CHECK_EQ(delivered.size(), 1U);
    CHECK(delivered[0] == payload);
}

TEST_CASE(fragmenter_channel_extra_parity) {
    Peer peer;
    peer.m_have_late_report = true;
    peer.m_peer_late_ratio = 0.0;
    peer.m_fec_stage = 0;
    auto payload = make_payload(3000, 1); // 3004B -> 3 数据片
    auto frags = run_fragmenter(peer, payload, 1350, 1 /*channel*/, 2 /*extra*/);
    CHECK_EQ(frags.size(), 5U); // 3 数据 + 2 通道固定冗余
    CHECK_EQ(frags[0].data_cnt, 3U);
    // probe 额外校验叠加
    frags = run_fragmenter(peer, payload, 1350, 0, 0, 3 /*probe*/);
    CHECK_EQ(frags.size(), 6U); // 3 数据 + 3 探测冗余
}

TEST_CASE(fragmenter_parity_capped_at_data_count) {
    Peer peer;
    peer.m_have_late_report = true;
    peer.m_peer_late_ratio = 0.0;
    peer.m_fec_stage = 0;
    // 单片消息 + 大量冗余 → 总校验片封顶为 data_count
    auto frags = run_fragmenter(peer, {'a'}, 1350, 0, 50, 50);
    CHECK_EQ(frags.size(), 2U); // 1 数据 + 1 校验（封顶）
    CHECK_EQ(frags[0].data_cnt, 1U);
}

TEST_CASE(fragmenter_message_id_increments) {
    Peer peer;
    peer.m_have_late_report = true;
    peer.m_peer_late_ratio = 0.0;
    peer.m_fec_stage = 0;
    auto f1 = run_fragmenter(peer, {'a'}, 1350);
    auto f2 = run_fragmenter(peer, {'b'}, 1350);
    CHECK_EQ(f1[0].msg_id, 1U);
    CHECK_EQ(f2[0].msg_id, 2U);
}

TEST_CASE(fragmenter_width_matches_mtu) {
    Peer peer;
    peer.m_have_late_report = true;
    peer.m_peer_late_ratio = 0.0;
    peer.m_fec_stage = 0;
    auto frags = run_fragmenter(peer, make_payload(200, 1), 1000);
    CHECK_EQ(frags[0].width, 1000 - 48); // frag_payload = mtu - 48
}
