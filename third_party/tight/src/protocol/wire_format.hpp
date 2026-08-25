#pragma once

// Internal wire-format constants and big-endian conversion helpers shared by
// the tight transport translation units. Not part of the public API.

#include <cstddef>
#include <cstdint>

namespace tight::tight_detail {

inline constexpr std::uint32_t kMagic = 0x54474854U;
inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kHeaderSize = 48;

// flags 字段高位：负载为 AES-256-GCM 密文（低位仍保留原语义，
// 对数据报文即数据分片数 data_cnt）
inline constexpr std::uint16_t kFlagEncrypted = 0x8000;

// 逻辑通道号打包在 reserved 字段（Data/Parity 报文中存 real_size）的高 4 位，
// 关键帧标记占 bit 11，real_size 占低 11 位：
//   reserved = (channel << 12) | (keyframe ? 0x0800 : 0) | (real_size & 0x07FF)
// real_size ≤ frag_payload = mtu - 48。默认 mtu 1350 → ≤1302 < 2047；使用
// 关键帧标记要求 mtu ≤ 2095（超出时 real_size 被截断，仅影响重组裁剪精度）。
// 通道最高 16 路，接收端据此识别所属通道。
inline constexpr std::uint16_t kChannelShift = 12;
inline constexpr std::uint16_t kChannelMask = 0xF000;
inline constexpr std::uint16_t kRealSizeMask = 0x07FF;

// reserved bit 11：该报文所属消息是关键帧（send_video keyframe=true 的消息，
// 其所有数据分片与校验片都置位）。接收端据此把关键帧报文排除在报文级延迟
// 直方图/迟到统计之外——大关键帧突发的串行化延迟（F/链路速率）是帧自身传输，
// 不应污染 P50 与迟到率（否则 delay-based 拥塞信号误触发、btl 误降）。
// 兼容性：旧读端会把 bit11 误读进 real_size（+2048），但 real_size 的使用处
// 均有 min(real_size, 实际分片大小) 钳制，无实际影响。
inline constexpr std::uint16_t kKeyframeMask = 0x0800;

inline std::uint8_t channel_of(std::uint16_t reserved) {
    return static_cast<std::uint8_t>((reserved & kChannelMask) >> kChannelShift);
}

// 报文是否属于关键帧消息
inline bool is_keyframe_packet(std::uint16_t reserved) {
    return (reserved & kKeyframeMask) != 0;
}

inline std::uint16_t to_be16(std::uint16_t v) {
    return static_cast<std::uint16_t>(((v & 0x00FFU) << 8) | ((v & 0xFF00U) >> 8));
}
inline std::uint32_t to_be32(std::uint32_t v) {
    return ((v & 0x000000FFU) << 24) | ((v & 0x0000FF00U) << 8)
         | ((v & 0x00FF0000U) >> 8)  | ((v & 0xFF000000U) >> 24);
}
inline std::uint64_t to_be64(std::uint64_t v) {
    return (static_cast<std::uint64_t>(to_be32(static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFULL))) << 32)
         | to_be32(static_cast<std::uint32_t>(v & 0xFFFFFFFFULL));
}

}
