#pragma once

// Shared helpers for tests that exercise the Data path (fragmenter ->
// reassembler) using the same wire conventions as transport.cpp.

#include "core/peer.hpp"
#include "channel/reassembler.hpp"
#include "protocol/wire_format.hpp"
#include "tight/types.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace test_helpers {

using tight::Bytes;
using tight::PacketHeader;
using tight::PacketType;
using tight::unix_millis;

inline PacketHeader make_data_header(std::uint32_t msg_id, std::uint32_t seq,
                                     std::uint16_t idx, std::uint16_t cnt,
                                     std::uint16_t data_cnt,
                                     std::uint16_t real_size,
                                     std::uint8_t channel = 0) {
    PacketHeader h;
    h.magic = tight::tight_detail::kMagic;
    h.version = tight::tight_detail::kVersion;
    h.type = (idx < data_cnt) ? PacketType::Data : PacketType::Parity;
    h.flags = data_cnt;
    h.sequence = seq;
    h.message_id = msg_id;
    h.fragment_index = idx;
    h.fragment_count = cnt;
    h.reserved = static_cast<std::uint16_t>(
        (real_size & tight::tight_detail::kRealSizeMask) |
        ((channel & 0x0F) << tight::tight_detail::kChannelShift));
    h.tick = static_cast<std::uint32_t>(unix_millis() & 0xFFFFFFFFULL);
    return h;
}

struct CapturedFragment {
    std::uint32_t msg_id;
    std::uint32_t seq; // 0 = Parity（不参与缺口跟踪）
    std::uint16_t idx;
    std::uint16_t cnt;
    std::uint16_t data_cnt;
    std::uint16_t real_size;
    std::uint16_t width;
    Bytes data;
};

// Mirrors transport.cpp's send_fragment: pads to width and feeds the
// datagram-equivalent into the Reassembler.
inline void feed_fragments_to_reassembler(
    tight::tight_detail::Peer& peer,
    const std::vector<CapturedFragment>& frags,
    const tight::tight_detail::Reassembler::DeliverCallback& deliver,
    const tight::tight_detail::Reassembler::LossCallback& on_message_loss = {}) {
    for (const auto& f : frags) {
        Bytes payload(f.width, 0);
        std::size_t n = std::min(f.data.size(),
                                 static_cast<std::size_t>(f.width));
        std::copy(f.data.begin(), f.data.begin() + n, payload.begin());
        auto h = make_data_header(f.msg_id, f.seq, f.idx, f.cnt, f.data_cnt,
                                  f.real_size);
        tight::tight_detail::Reassembler::handle_data(
            peer, h, payload, 10000, 4.0, 0, 64 * 1024,
            std::chrono::milliseconds(1000), deliver, on_message_loss);
    }
}

} // namespace test_helpers
