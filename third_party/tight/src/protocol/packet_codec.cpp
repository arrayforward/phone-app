#include "tight/packet_codec.hpp"

#include "protocol/crc32.hpp"
#include "protocol/wire_format.hpp"

#include <cstring>

namespace tight {

using namespace tight_detail;

std::size_t PacketCodec::encode_header_to(const PacketHeader& header, std::uint8_t* out) {
    std::uint8_t* p = out;

    auto put32 = [&](std::size_t off, std::uint32_t v) {
        std::uint32_t be = to_be32(v);
        std::memcpy(p + off, &be, 4);
    };
    auto put16 = [&](std::size_t off, std::uint16_t v) {
        std::uint16_t be = to_be16(v);
        std::memcpy(p + off, &be, 2);
    };
    auto put64 = [&](std::size_t off, std::uint64_t v) {
        std::uint64_t be = to_be64(v);
        std::memcpy(p + off, &be, 8);
    };

    put32(0, header.magic);
    p[4] = header.version;
    p[5] = static_cast<std::uint8_t>(header.type);
    put16(6, header.flags);
    put32(8, header.client_id);
    put64(12, header.session_id);
    put32(20, header.sequence);
    put32(24, header.acknowledgment);
    put32(28, header.message_id);
    put16(32, header.fragment_index);
    put16(34, header.fragment_count);
    // 使用 header.payload_size 而非 payload.size()：加密路径下头部先定稿
    // （密文长度），AAD 重编码时需与线上字节一致
    put16(36, header.payload_size);
    put16(38, header.reserved);
    put32(40, header.tick);
    put32(44, 0);
    return kHeaderSize;
}

void PacketCodec::finalize_crc(std::uint8_t* datagram, std::size_t size) {
    if (size < kHeaderSize) return;
    std::memset(datagram + 44, 0, 4);
    // 流式 CRC：头 44 字节（含已清零的 CRC 域）+ 负载，一步完成
    std::uint32_t crc = crc32_update(0xFFFFFFFFU, datagram, size);
    std::uint32_t be = to_be32(crc ^ 0xFFFFFFFFU);
    std::memcpy(datagram + 44, &be, 4);
}

std::size_t PacketCodec::encode_to(const PacketHeader& header, const Bytes& payload,
                                   std::uint8_t* out) {
    std::size_t total = encode_header_to(header, out);
    if (!payload.empty()) {
        std::memcpy(out + kHeaderSize, payload.data(), payload.size());
        total += payload.size();
    }
    finalize_crc(out, total);
    return total;
}

Bytes PacketCodec::encode(const PacketHeader& header, const Bytes& payload) {
    Bytes buf(kHeaderSize + payload.size());
    encode_to(header, payload, buf.data());
    return buf;
}

bool PacketCodec::decode(const std::uint8_t* p, std::size_t size,
                         PacketHeader& header, Bytes& payload) {
    if (size < kHeaderSize) return false;

    auto get32 = [&](std::size_t off) {
        std::uint32_t v;
        std::memcpy(&v, p + off, 4);
        return to_be32(v);
    };
    auto get16 = [&](std::size_t off) {
        std::uint16_t v;
        std::memcpy(&v, p + off, 2);
        return to_be16(v);
    };
    auto get64 = [&](std::size_t off) {
        std::uint32_t lo = get32(off);
        std::uint32_t hi = get32(off + 4);
        return (static_cast<std::uint64_t>(hi) << 32) | lo;
    };

    std::uint32_t magic = get32(0);
    if (magic != kMagic) return false;
    std::uint8_t version = p[4];
    if (version != kVersion) return false;

    header.magic = magic;
    header.version = version;
    header.type = static_cast<PacketType>(p[5]);
    header.flags = get16(6);
    header.client_id = get32(8);
    header.session_id = get64(12);
    header.sequence = get32(20);
    header.acknowledgment = get32(24);
    header.message_id = get32(28);
    header.fragment_index = get16(32);
    header.fragment_count = get16(34);
    std::uint16_t payload_size = get16(36);
    header.payload_size = payload_size;
    header.reserved = get16(38);
    header.tick = get32(40);
    header.checksum = get32(44);

    if (size < kHeaderSize + payload_size) return false;

    // 流式 CRC 校验：头 44 字节 + 4 个零字节 + 负载（免 tmp 拷贝）
    std::uint32_t crc = crc32_update(0xFFFFFFFFU, p, 44);
    const std::uint8_t zeros[4] = {0, 0, 0, 0};
    crc = crc32_update(crc, zeros, 4);
    crc = crc32_update(crc, p + kHeaderSize, payload_size);
    if ((crc ^ 0xFFFFFFFFU) != header.checksum) return false;

    payload.assign(p + kHeaderSize, p + kHeaderSize + payload_size);
    return true;
}

bool PacketCodec::decode(const Bytes& datagram, PacketHeader& header, Bytes& payload) {
    return decode(datagram.data(), datagram.size(), header, payload);
}

std::uint32_t PacketCodec::crc32(const std::uint8_t* data, std::size_t size) {
    return crc32_compute(data, size);
}

}
