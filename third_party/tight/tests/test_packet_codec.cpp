#include "test_framework.hpp"

#include "protocol/wire_format.hpp"
#include "tight/packet_codec.hpp"

#include <algorithm>
#include <cstdint>

using namespace tight;

namespace {

PacketHeader make_header() {
    PacketHeader h;
    h.magic = tight_detail::kMagic;
    h.version = tight_detail::kVersion;
    h.type = PacketType::Data;
    h.flags = 3;
    h.client_id = 0xDEADBEEFU;
    h.session_id = 0x0102030405060708ULL;
    h.sequence = 42;
    h.acknowledgment = 7;
    h.message_id = 12345;
    h.fragment_index = 1;
    h.fragment_count = 3;
    h.payload_size = 9;
    h.reserved = 0x1234;
    h.tick = 987654321;
    h.checksum = 0;
    return h;
}

Bytes make_payload() {
    Bytes p(9);
    for (std::size_t i = 0; i < p.size(); ++i) p[i] = static_cast<std::uint8_t>(i * 17);
    return p;
}

bool header_equal(const PacketHeader& a, const PacketHeader& b) {
    return a.magic == b.magic && a.version == b.version && a.type == b.type &&
           a.flags == b.flags && a.client_id == b.client_id &&
           a.session_id == b.session_id && a.sequence == b.sequence &&
           a.acknowledgment == b.acknowledgment &&
           a.message_id == b.message_id &&
           a.fragment_index == b.fragment_index &&
           a.fragment_count == b.fragment_count &&
           a.payload_size == b.payload_size && a.reserved == b.reserved &&
           a.tick == b.tick; // checksum 由 decode 成功与否验证，不做字段比较
}

} // namespace

TEST_CASE(packet_codec_roundtrip) {
    auto h = make_header();
    auto p = make_payload();
    auto wire = PacketCodec::encode(h, p);
    CHECK_EQ(wire.size(), tight_detail::kHeaderSize + p.size());

    PacketHeader out_h;
    Bytes out_p;
    CHECK(PacketCodec::decode(wire, out_h, out_p));
    CHECK(header_equal(h, out_h));
    CHECK(out_p == p);
    CHECK_NE(out_h.checksum, 0U);
}

TEST_CASE(packet_codec_large_payload_roundtrip) {
    PacketHeader h = make_header();
    Bytes p(4096);
    for (std::size_t i = 0; i < p.size(); ++i)
        p[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
    h.payload_size = static_cast<std::uint16_t>(p.size());
    auto wire = PacketCodec::encode(h, p);
    PacketHeader out_h;
    Bytes out_p;
    CHECK(PacketCodec::decode(wire, out_h, out_p));
    CHECK(out_p == p);
}

TEST_CASE(packet_codec_detects_payload_tamper) {
    auto h = make_header();
    auto p = make_payload();
    auto wire = PacketCodec::encode(h, p);
    wire[tight_detail::kHeaderSize + 3] ^= 0xFF;
    PacketHeader out_h;
    Bytes out_p;
    CHECK(!PacketCodec::decode(wire, out_h, out_p));
}

TEST_CASE(packet_codec_detects_header_tamper) {
    auto h = make_header();
    auto p = make_payload();
    auto wire = PacketCodec::encode(h, p);
    wire[20] ^= 0x01; // sequence field
    PacketHeader out_h;
    Bytes out_p;
    CHECK(!PacketCodec::decode(wire, out_h, out_p));
}

TEST_CASE(packet_codec_rejects_bad_magic) {
    auto wire = PacketCodec::encode(make_header(), make_payload());
    wire[0] ^= 0xFF;
    PacketHeader out_h;
    Bytes out_p;
    CHECK(!PacketCodec::decode(wire, out_h, out_p));
}

TEST_CASE(packet_codec_rejects_bad_version) {
    auto wire = PacketCodec::encode(make_header(), make_payload());
    wire[4] = 99;
    PacketHeader out_h;
    Bytes out_p;
    CHECK(!PacketCodec::decode(wire, out_h, out_p));
}

TEST_CASE(packet_codec_rejects_truncated) {
    auto wire = PacketCodec::encode(make_header(), make_payload());
    wire.resize(tight_detail::kHeaderSize - 1);
    PacketHeader out_h;
    Bytes out_p;
    CHECK(!PacketCodec::decode(wire, out_h, out_p));

    wire = PacketCodec::encode(make_header(), make_payload());
    wire.resize(tight_detail::kHeaderSize + 4); // payload_size says 9
    CHECK(!PacketCodec::decode(wire, out_h, out_p));
}

TEST_CASE(packet_codec_rejects_empty) {
    Bytes wire;
    PacketHeader out_h;
    Bytes out_p;
    CHECK(!PacketCodec::decode(wire, out_h, out_p));
}

TEST_CASE(packet_codec_encode_to_matches_encode) {
    auto h = make_header();
    auto p = make_payload();
    auto expected = PacketCodec::encode(h, p);
    std::vector<std::uint8_t> buf(tight_detail::kHeaderSize + p.size());
    std::size_t n = PacketCodec::encode_to(h, p, buf.data());
    CHECK_EQ(n, expected.size());
    CHECK(buf == expected);
}

TEST_CASE(packet_codec_header_then_finalize) {
    auto h = make_header();
    auto p = make_payload();
    std::vector<std::uint8_t> buf(tight_detail::kHeaderSize + p.size());
    std::size_t n = PacketCodec::encode_header_to(h, buf.data());
    CHECK_EQ(n, tight_detail::kHeaderSize);
    CHECK_EQ(*reinterpret_cast<std::uint32_t*>(&buf[44]), 0U); // crc zeroed
    std::copy(p.begin(), p.end(), buf.begin() + tight_detail::kHeaderSize);
    PacketCodec::finalize_crc(buf.data(), buf.size());

    PacketHeader out_h;
    Bytes out_p;
    CHECK(PacketCodec::decode(buf.data(), buf.size(), out_h, out_p));
    CHECK(header_equal(h, out_h));
    CHECK(out_p == p);
}

TEST_CASE(packet_codec_empty_payload) {
    PacketHeader h = make_header();
    h.payload_size = 0;
    auto wire = PacketCodec::encode(h, {});
    CHECK_EQ(wire.size(), tight_detail::kHeaderSize);
    PacketHeader out_h;
    Bytes out_p;
    CHECK(PacketCodec::decode(wire, out_h, out_p));
    CHECK(out_p.empty());
}

TEST_CASE(packet_codec_big_endian_layout) {
    PacketHeader h = make_header();
    h.client_id = 0x01020304U;
    h.session_id = 0x0102030405060708ULL;
    auto wire = PacketCodec::encode(h, {});
    // 32 位字段按大端写线：0x01020304 -> 01 02 03 04
    CHECK_EQ(wire[8], 0x01);
    CHECK_EQ(wire[9], 0x02);
    CHECK_EQ(wire[10], 0x03);
    CHECK_EQ(wire[11], 0x04);
    // 64 位 session_id：先低 32 位（BE）后高 32 位（BE）
    // lo=0x05060708 -> 05 06 07 08，hi=0x01020304 -> 01 02 03 04
    CHECK_EQ(wire[12], 0x05);
    CHECK_EQ(wire[13], 0x06);
    CHECK_EQ(wire[14], 0x07);
    CHECK_EQ(wire[15], 0x08);
    CHECK_EQ(wire[16], 0x01);
    CHECK_EQ(wire[17], 0x02);
    CHECK_EQ(wire[18], 0x03);
    CHECK_EQ(wire[19], 0x04);
}
