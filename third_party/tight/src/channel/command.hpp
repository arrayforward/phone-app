#pragma once

// Internal command channel: ordered delivery of Command packets (control and
// button/key information). Command packets fit in a single datagram, so they
// bypass the Data path entirely (no fragmentation, no reassembly, no FEC).
//
// Ordering: every Command carries a per-peer sequence. In-order packets are
// delivered immediately; out-of-order packets are held for at most 3 RTT
// waiting for the gap to fill. When the wait expires, the gap is skipped and
// everything held is delivered in order; a skipped packet that arrives later
// is dropped. Not part of the public API.

#include "tight/types.hpp"

#include <cstdint>
#include <vector>

namespace tight::tight_detail {

struct Peer;

class CommandChannel {
public:
    // Handles an incoming Command packet. Returns the payloads ready for
    // in-order delivery (empty when the packet was held or dropped).
    static std::vector<Bytes> handle(Peer& peer, const PacketHeader& header,
                                     const Bytes& payload, std::uint32_t rtt_us);

    // Periodic expiry check: if the reorder wait exceeded 3 RTT, skips the
    // missing sequence(s) and returns the held payloads for in-order
    // delivery.
    static std::vector<Bytes> flush_expired(Peer& peer, std::uint32_t rtt_us);

    // Resets both directions of the command channel (on (re)handshake).
    static void reset(Peer& peer);

private:
    static constexpr std::uint32_t kMaxWaitRtt = 3;

    static void drain_locked(Peer& peer, std::vector<Bytes>& out);
    static void expire_locked(Peer& peer, std::uint32_t rtt_us,
                              std::chrono::steady_clock::time_point now,
                              std::vector<Bytes>& out);
};

}
