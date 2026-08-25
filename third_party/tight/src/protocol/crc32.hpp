#pragma once

// Internal CRC32 (IEEE 802.3, table-driven) declaration. The implementation
// lives in crc32.cpp. Not part of the public API; the public entry point is
// PacketCodec::crc32.

#include <cstddef>
#include <cstdint>

namespace tight::tight_detail {

// 一次性计算（内部状态 0xFFFFFFFF 起，结果异或 0xFFFFFFFF 输出）
std::uint32_t crc32_compute(const std::uint8_t* data, std::size_t size);

// 流式更新：crc 为前一次的内部状态（未做最终异或），首段传 0xFFFFFFFF。
// 全部段喂完后自行异或 0xFFFFFFFF 得到最终值。用于免拷贝校验
// （报文头 44 字节 + 4 个零字节 + 负载，无需拼接临时缓冲）。
std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t* data, std::size_t size);

}
