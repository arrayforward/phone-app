#include "test_framework.hpp"

#include "channel/command.hpp"
#include "core/peer.hpp"

#include <chrono>
#include <cstdint>
#include <thread>

using namespace tight;
using namespace tight::tight_detail;
using namespace std::chrono_literals;

namespace {

Bytes make_payload(std::uint8_t tag) {
    Bytes p(4);
    for (auto& b : p) b = tag;
    return p;
}

PacketHeader make_cmd_header(std::uint32_t seq) {
    PacketHeader h;
    h.type = PacketType::Command;
    h.sequence = seq;
    return h;
}

} // namespace

TEST_CASE(command_in_order_delivery) {
    Peer peer;
    std::vector<Bytes> out;
    out = CommandChannel::handle(peer, make_cmd_header(1), make_payload(1), 10000);
    CHECK_EQ(out.size(), 1U);
    CHECK(out[0] == make_payload(1));
    out = CommandChannel::handle(peer, make_cmd_header(2), make_payload(2), 10000);
    CHECK_EQ(out.size(), 1U);
    out = CommandChannel::handle(peer, make_cmd_header(3), make_payload(3), 10000);
    CHECK_EQ(out.size(), 1U);
}

TEST_CASE(command_out_of_order_held_until_gap_fills) {
    Peer peer;
    auto out = CommandChannel::handle(peer, make_cmd_header(1), make_payload(1), 10000);
    CHECK_EQ(out.size(), 1U);
    out = CommandChannel::handle(peer, make_cmd_header(3), make_payload(3), 10000);
    CHECK(out.empty()); // held
    out = CommandChannel::handle(peer, make_cmd_header(2), make_payload(2), 10000);
    CHECK_EQ(out.size(), 2U); // gap filled, both delivered in order
    CHECK(out[0] == make_payload(2));
    CHECK(out[1] == make_payload(3));
}

TEST_CASE(command_gap_expires_and_skips) {
    Peer peer;
    auto out = CommandChannel::handle(peer, make_cmd_header(1), make_payload(1), 10000);
    CHECK_EQ(out.size(), 1U);
    out = CommandChannel::handle(peer, make_cmd_header(3), make_payload(3), 10000);
    CHECK(out.empty());
    // 3 RTT = 30ms；等待超时后跳号投递
    std::this_thread::sleep_for(60ms);
    out = CommandChannel::flush_expired(peer, 10000);
    CHECK_EQ(out.size(), 1U);
    CHECK(out[0] == make_payload(3));
    // 迟到包被丢弃
    out = CommandChannel::handle(peer, make_cmd_header(2), make_payload(2), 10000);
    CHECK(out.empty());
}

TEST_CASE(command_duplicate_dropped) {
    Peer peer;
    auto out = CommandChannel::handle(peer, make_cmd_header(1), make_payload(1), 10000);
    CHECK_EQ(out.size(), 1U);
    out = CommandChannel::handle(peer, make_cmd_header(1), make_payload(1), 10000);
    CHECK(out.empty());
    out = CommandChannel::handle(peer, make_cmd_header(0), make_payload(0), 10000);
    CHECK(out.empty());
}

TEST_CASE(command_reset_clears_state) {
    Peer peer;
    // First packet initializes the base and is delivered immediately
    auto out = CommandChannel::handle(peer, make_cmd_header(2), make_payload(2), 10000);
    CHECK_EQ(out.size(), 1U);
    // Already-delivered sequence is dropped
    out = CommandChannel::handle(peer, make_cmd_header(1), make_payload(1), 10000);
    CHECK(out.empty());
    CommandChannel::reset(peer);
    out = CommandChannel::handle(peer, make_cmd_header(5), make_payload(5), 10000);
    CHECK_EQ(out.size(), 1U); // fresh base after reset
}
