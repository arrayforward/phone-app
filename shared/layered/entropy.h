// 增强层残差熵编码（协议 docs/protocol.md §3.4）：零段跳过 + 逐平面最优 k
// Rice 码。纯 C++，无平台依赖，host 可单测（layered_test.cpp）。
//
// 设计要点：
// - 残差字节 sym = clamp(orig−recon, −128,127) + 128 ∈ [0,255]，128 = 零残差。
// - 帧字节流 = Y 平面 W×H + U 平面 (W/2)×(H/2) + V 平面同，共 W*H*3/2 字节。
// - 按 4096B 分段，全零残差段只在位图里标 1、码流无数据（典型帧大量命中）。
// - Rice(k)：zigzag z ∈ [0,255]；q=z>>k 个 1 + 一个 0 + k 位余数（MSB 优先）；
//   q ≥ 32 逃逸：32 个 1 + z 的 8bit 原码（无分隔位无余数位）。
// - k 逐帧逐平面静态选最优（遍历 0..8 估代价），写入帧头，无自适应同步问题。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace layered {

// ---- zigzag 映射 ----
// delta ∈ [-128,127] → z ∈ [0,255]
inline std::uint32_t zigzag(int delta) {
    return delta >= 0 ? (std::uint32_t)(2 * delta) : (std::uint32_t)(-2 * delta - 1);
}
inline int unzigzag(std::uint32_t z) {
    return (z & 1) ? -(int)((z + 1) / 2) : (int)(z / 2);
}

// ---- MSB 优先位流 ----
class BitWriter {
public:
    void put_bit(int b);
    void put_bits(std::uint32_t v, int n);  // MSB 优先写 n 位
    const std::vector<std::uint8_t>& bytes() const { return m_out; }
    std::size_t bit_count() const { return m_out.size() * 8 - (8 - m_used) % 8; }
private:
    std::vector<std::uint8_t> m_out;
    int m_used = 0;  // 当前字节已用位数
};

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size) : m_data(data), m_size(size) {}
    int get_bit();                       // 超界返回 0（容错：Rice 流尾部可依赖）
    std::uint32_t get_bits(int n);       // MSB 优先读 n 位（n ≤ 32）
private:
    const std::uint8_t* m_data;
    std::size_t m_size;
    std::size_t m_pos = 0;               // 位偏移
};

// ---- Rice 单值 ----
void rice_encode(BitWriter& w, std::uint32_t z, int k);
std::uint32_t rice_decode(BitReader& r, int k);

// ---- 帧级编解码（载荷部分，不含 9 字节消息头）----
// sym：残差符号平面（W*H*3/2 字节，Y+U+V planar），128 = 零残差。
// 帧头：ver(1) | k_y<<4|k_u(1) | k_v<<4|0(1) | 零段位图(ceil(nseg/8))。
// 返回 false = 超尺寸护栏或输入非法。W、H 必须为偶数。
bool encode_enhancement(const std::uint8_t* sym, int w, int h,
                        std::size_t max_bytes, std::vector<std::uint8_t>& out);
bool decode_enhancement(const std::uint8_t* data, std::size_t size,
                        int w, int h, std::uint8_t* sym_out);

// 残差平面总字节数（W、H 为偶数）
inline std::size_t frame_bytes(int w, int h) {
    return (std::size_t)w * h * 3 / 2;
}

}  // namespace layered
