#pragma once

// Public packet codec for the tight wire format. The implementation lives in
// tight/packet_codec.cpp; CRC32 is implemented in tight/crc32.cpp.

#include "tight/types.hpp"

#include <cstddef>
#include <cstdint>

namespace tight {

class PacketCodec {
public:
    static Bytes encode(const PacketHeader& header, const Bytes& payload);
    static bool decode(const Bytes& datagram, PacketHeader& header, Bytes& payload);
    static std::uint32_t crc32(const std::uint8_t* data, std::size_t size);

    // 零堆分配变体：
    // encode_to 写入调用方缓冲区（容量需 ≥ 48 + payload.size()），返回总长。
    // decode 直接从指针/长度解码，流式 CRC 校验，免去 datagram 与 tmp 拷贝。
    static std::size_t encode_to(const PacketHeader& header, const Bytes& payload,
                                 std::uint8_t* out);
    static bool decode(const std::uint8_t* data, std::size_t size,
                       PacketHeader& header, Bytes& payload);

    // 更细粒度（单缓冲构建线上报文用）：
    // encode_header_to 只写 48 字节报文头（CRC 域置零），返回 48。
    // finalize_crc 对已装配好的整报文（头+负载）计算 CRC 并写入 44-47。
    static std::size_t encode_header_to(const PacketHeader& header, std::uint8_t* out);
    static void finalize_crc(std::uint8_t* datagram, std::size_t size);
};

} // namespace tight
