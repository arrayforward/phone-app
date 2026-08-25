#include "channel/command.hpp"

#include "core/peer.hpp"

#include <chrono>
#include <mutex>
#include <utility>

namespace tight::tight_detail {

std::vector<Bytes> CommandChannel::handle(Peer& peer, const PacketHeader& header,
                                          const Bytes& payload, std::uint32_t rtt_us) {
    std::vector<Bytes> out;
    std::uint32_t seq = header.sequence;
    if (seq == 0) return out;
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(peer.m_mu);
    if (!peer.m_cmd_initialized) {
        peer.m_cmd_next_expected = seq;
        peer.m_cmd_initialized = true;
    }
    if (seq < peer.m_cmd_next_expected) {
        return out;  // already delivered or skipped: drop
    }
    if (seq == peer.m_cmd_next_expected) {
        out.push_back(payload);
        ++peer.m_cmd_next_expected;
        drain_locked(peer, out);
    } else {
        // Gap detected: hold the packet and wait up to 3 RTT for the missing
        // sequence(s).
        if (peer.m_cmd_held.empty()) peer.m_cmd_gap_since = now;
        peer.m_cmd_held.emplace(seq, payload);
        expire_locked(peer, rtt_us, now, out);
    }
    return out;
}

std::vector<Bytes> CommandChannel::flush_expired(Peer& peer, std::uint32_t rtt_us) {
    std::vector<Bytes> out;
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(peer.m_mu);
    expire_locked(peer, rtt_us, now, out);
    return out;
}

void CommandChannel::reset(Peer& peer) {
    std::lock_guard<std::mutex> lock(peer.m_mu);
    peer.m_cmd_seq_out = 1;
    peer.m_cmd_next_expected = 0;
    peer.m_cmd_initialized = false;
    peer.m_cmd_held.clear();
    peer.m_cmd_gap_since = {};
}

void CommandChannel::drain_locked(Peer& peer, std::vector<Bytes>& out) {
    while (true) {
        auto it = peer.m_cmd_held.find(peer.m_cmd_next_expected);
        if (it == peer.m_cmd_held.end()) break;
        out.push_back(std::move(it->second));
        peer.m_cmd_held.erase(it);
        ++peer.m_cmd_next_expected;
    }
}

void CommandChannel::expire_locked(Peer& peer, std::uint32_t rtt_us,
                                   std::chrono::steady_clock::time_point now,
                                   std::vector<Bytes>& out) {
    if (peer.m_cmd_held.empty()) return;
    std::int64_t wait_us = static_cast<std::int64_t>(kMaxWaitRtt) *
                           static_cast<std::int64_t>(rtt_us > 0 ? rtt_us : 10000);
    if (now - peer.m_cmd_gap_since < std::chrono::microseconds(wait_us)) return;
    // The missing packet(s) did not arrive within 3 RTT: skip the gap and
    // deliver everything held, in order. Late arrivals of the skipped
    // sequences are dropped (seq < m_cmd_next_expected).
    peer.m_cmd_next_expected = peer.m_cmd_held.begin()->first;
    drain_locked(peer, out);
    // A further gap may remain: restart its wait window.
    if (!peer.m_cmd_held.empty()) peer.m_cmd_gap_since = now;
}

}
