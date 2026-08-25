#include "protocol/crc32.hpp"

#include <array>
#include <mutex>

namespace tight::tight_detail {

namespace {

std::once_flag g_crc_table_once;
std::array<std::uint32_t, 256> g_crc_table{};

void init_crc_table() {
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        }
        g_crc_table[i] = c;
    }
}

}

std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t* data, std::size_t size) {
    std::call_once(g_crc_table_once, init_crc_table);
    for (std::size_t i = 0; i < size; ++i) {
        crc = g_crc_table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8);
    }
    return crc;
}

std::uint32_t crc32_compute(const std::uint8_t* data, std::size_t size) {
    return crc32_update(0xFFFFFFFFU, data, size) ^ 0xFFFFFFFFU;
}

}
