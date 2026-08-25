#include "test_framework.hpp"

#include "protocol/crc32.hpp"
#include "tight/packet_codec.hpp"

#include <cstdint>
#include <cstring>
#include <string>

using namespace tight;

TEST_CASE(crc32_known_vector) {
    // IEEE 802.3 CRC-32 standard check value
    const std::string msg = "123456789";
    CHECK_EQ(tight_detail::crc32_compute(
                 reinterpret_cast<const std::uint8_t*>(msg.data()), msg.size()),
             0xCBF43926U);
}

TEST_CASE(crc32_empty) {
    CHECK_EQ(tight_detail::crc32_compute(nullptr, 0), 0x00000000U);
}

TEST_CASE(crc32_streaming_matches_single_shot) {
    const std::string msg = "The quick brown fox jumps over the lazy dog";
    auto whole = tight_detail::crc32_compute(
        reinterpret_cast<const std::uint8_t*>(msg.data()), msg.size());

    std::uint32_t crc = 0xFFFFFFFFU;
    std::size_t pos = 0;
    while (pos < msg.size()) {
        std::size_t take = (pos + 7 < msg.size()) ? 7 : msg.size() - pos;
        crc = tight_detail::crc32_update(
            crc, reinterpret_cast<const std::uint8_t*>(msg.data() + pos), take);
        pos += take;
    }
    CHECK_EQ(crc ^ 0xFFFFFFFFU, whole);
}

TEST_CASE(crc32_public_api) {
    const std::string msg = "123456789";
    CHECK_EQ(PacketCodec::crc32(
                 reinterpret_cast<const std::uint8_t*>(msg.data()), msg.size()),
             0xCBF43926U);
}

TEST_CASE(crc32_differs_for_similar_input) {
    const std::string a = "aaaa";
    const std::string b = "aaab";
    CHECK_NE(tight_detail::crc32_compute(
                 reinterpret_cast<const std::uint8_t*>(a.data()), a.size()),
             tight_detail::crc32_compute(
                 reinterpret_cast<const std::uint8_t*>(b.data()), b.size()));
}
