#include "entropy.h"

#include <cstring>

namespace layered {

namespace {

constexpr int kSegLen = 4096;        // 零段长度（字节）
constexpr std::uint8_t kVer = 0x01;  // 载荷格式版本
constexpr int kMaxK = 8;
constexpr std::uint32_t kEscQ = 32;  // Rice 逃逸阈值：q ≥ 32 → 32 个 1 + 8bit 原码

}  // namespace

void BitWriter::put_bit(int b) {
    if (m_used == 0) m_out.push_back(0);
    if (b) m_out.back() |= (std::uint8_t)(0x80 >> m_used);
    m_used = (m_used + 1) & 7;
}

void BitWriter::put_bits(std::uint32_t v, int n) {
    for (int i = n - 1; i >= 0; --i) put_bit((v >> i) & 1);
}

int BitReader::get_bit() {
    if (m_pos >= m_size * 8) return 0;
    int b = (m_data[m_pos >> 3] >> (7 - (m_pos & 7))) & 1;
    ++m_pos;
    return b;
}

std::uint32_t BitReader::get_bits(int n) {
    std::uint32_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 1) | (std::uint32_t)get_bit();
    return v;
}

void rice_encode(BitWriter& w, std::uint32_t z, int k) {
    std::uint32_t q = z >> k;
    if (q >= kEscQ) {
        for (std::uint32_t i = 0; i < kEscQ; ++i) w.put_bit(1);
        w.put_bits(z, 8);
        return;
    }
    for (std::uint32_t i = 0; i < q; ++i) w.put_bit(1);
    w.put_bit(0);
    if (k > 0) w.put_bits(z & ((1u << k) - 1), k);
}

std::uint32_t rice_decode(BitReader& r, int k) {
    std::uint32_t q = 0;
    while (r.get_bit()) {
        if (++q == kEscQ) return r.get_bits(8);  // 逃逸
    }
    std::uint32_t rem = k > 0 ? r.get_bits(k) : 0;
    return (q << k) | rem;
}

// 单字节在 k 下的编码位数估计
static inline std::uint32_t rice_cost(std::uint32_t z, int k) {
    std::uint32_t q = z >> k;
    return q >= kEscQ ? kEscQ + 8 : q + 1 + (std::uint32_t)k;
}

// 逐平面选最优 k：先一次扫描建 z 直方图（256 桶），再在直方图上遍历 0..8
// 估代价（并列取小 k）。避免 9 次全平面扫描。
static int choose_k(const std::uint8_t* sym, std::size_t begin, std::size_t end,
                    const std::uint8_t* seg_skip) {
    std::uint32_t hist[256] = {};
    for (std::size_t i = begin; i < end; ++i) {
        if (seg_skip[i / kSegLen]) continue;
        ++hist[zigzag((int)sym[i] - 128)];
    }
    std::uint64_t best = ~0ull;
    int best_k = 0;
    for (int k = 0; k <= kMaxK; ++k) {
        std::uint64_t cost = 0;
        for (int z = 0; z < 256; ++z)
            cost += (std::uint64_t)hist[z] * rice_cost((std::uint32_t)z, k);
        if (cost < best) {
            best = cost;
            best_k = k;
        }
    }
    return best_k;
}

bool encode_enhancement(const std::uint8_t* sym, int w, int h,
                        std::size_t max_bytes, std::vector<std::uint8_t>& out) {
    if (!sym || w <= 0 || h <= 0 || (w & 1) || (h & 1)) return false;
    const std::size_t n = frame_bytes(w, h);
    const std::size_t nseg = (n + kSegLen - 1) / kSegLen;
    const std::size_t bitmap_bytes = (nseg + 7) / 8;

    // 零段位图（MSB 优先）
    std::vector<std::uint8_t> bitmap(bitmap_bytes, 0);
    for (std::size_t s = 0; s < nseg; ++s) {
        std::size_t begin = s * kSegLen;
        std::size_t end = begin + kSegLen < n ? begin + kSegLen : n;
        bool zero = true;
        for (std::size_t i = begin; i < end; ++i) {
            if (sym[i] != 128) { zero = false; break; }
        }
        if (zero) bitmap[s >> 3] |= (std::uint8_t)(0x80 >> (s & 7));
    }
    auto skipped = [&](std::size_t seg) { return (bitmap[seg >> 3] >> (7 - (seg & 7))) & 1; };

    // 逐平面最优 k
    const std::size_t y_end = (std::size_t)w * h;
    const std::size_t u_end = y_end + (std::size_t)(w / 2) * (h / 2);
    std::vector<std::uint8_t> skip_vec(bitmap);
    int k_y = choose_k(sym, 0, y_end, skip_vec.data());
    int k_u = choose_k(sym, y_end, u_end, skip_vec.data());
    int k_v = choose_k(sym, u_end, n, skip_vec.data());

    // 帧头 + Rice 码流
    BitWriter bw;
    for (std::size_t i = 0; i < n; ++i) {
        if (skipped(i / kSegLen)) continue;
        int k = i < y_end ? k_y : (i < u_end ? k_u : k_v);
        rice_encode(bw, zigzag((int)sym[i] - 128), k);
    }

    std::size_t total = 3 + bitmap_bytes + bw.bytes().size();
    if (total > max_bytes) return false;  // 尺寸护栏：弃发该帧增强
    out.clear();
    out.reserve(total);
    out.push_back(kVer);
    out.push_back((std::uint8_t)((k_y << 4) | k_u));
    out.push_back((std::uint8_t)(k_v << 4));
    out.insert(out.end(), bitmap.begin(), bitmap.end());
    out.insert(out.end(), bw.bytes().begin(), bw.bytes().end());
    return true;
}

bool decode_enhancement(const std::uint8_t* data, std::size_t size,
                        int w, int h, std::uint8_t* sym_out) {
    if (!data || !sym_out || w <= 0 || h <= 0 || (w & 1) || (h & 1)) return false;
    const std::size_t n = frame_bytes(w, h);
    const std::size_t nseg = (n + kSegLen - 1) / kSegLen;
    const std::size_t bitmap_bytes = (nseg + 7) / 8;
    if (size < 3 + bitmap_bytes) return false;
    if (data[0] != kVer) return false;

    int k_y = data[1] >> 4, k_u = data[1] & 0xF, k_v = data[2] >> 4;
    if (k_y > kMaxK || k_u > kMaxK || k_v > kMaxK) return false;
    const std::uint8_t* bitmap = data + 3;
    auto skipped = [&](std::size_t seg) { return (bitmap[seg >> 3] >> (7 - (seg & 7))) & 1; };

    const std::size_t y_end = (std::size_t)w * h;
    const std::size_t u_end = y_end + (std::size_t)(w / 2) * (h / 2);

    BitReader br(data + 3 + bitmap_bytes, size - 3 - bitmap_bytes);
    for (std::size_t s = 0; s < nseg; ++s) {
        std::size_t begin = s * kSegLen;
        std::size_t end = begin + kSegLen < n ? begin + kSegLen : n;
        if (skipped(s)) {
            std::memset(sym_out + begin, 128, end - begin);
            continue;
        }
        for (std::size_t i = begin; i < end; ++i) {
            int k = i < y_end ? k_y : (i < u_end ? k_u : k_v);
            sym_out[i] = (std::uint8_t)(128 + unzigzag(rice_decode(br, k)));
        }
    }
    return true;
}

}  // namespace layered
