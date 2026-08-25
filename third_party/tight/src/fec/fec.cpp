#include "tight/fec.hpp"

#include "fec/fec_internal.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace tight {

namespace {

// GF(2^8) 域运算查表实现：exp/log 双表，域元素乘法转化为指数加法。
// 本原多项式 x^8 + x^4 + x^3 + x^2 + 1 (0x11D)，生成元为 2。
struct GfTables {
    std::array<std::uint8_t, 512> exp{};   // 指数表（双倍长度免取模）
    std::array<std::uint8_t, 256> log{};   // 对数表

    GfTables() {
        std::uint16_t x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[i] = static_cast<std::uint8_t>(x);
            log[x] = static_cast<std::uint8_t>(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;     // 约化：减去本原多项式
        }
        for (int i = 255; i < 512; ++i) exp[i] = exp[i - 255];
    }
};

const GfTables& gf() {
    static GfTables t;   // 首次使用时构建一次
    return t;
}

// 域乘法（0 元素短路，避免 log[0]）
inline std::uint8_t gf_mul(std::uint8_t a, std::uint8_t b) {
    if (a == 0 || b == 0) return 0;
    return gf().exp[gf().log[a] + gf().log[b]];
}

// 域幂运算 a^e
inline std::uint8_t gf_pow(std::uint8_t a, int e) {
    if (e == 0) return 1;
    if (a == 0) return 0;
    return gf().exp[(gf().log[a] * e) % 255];
}

// 域乘法逆元：a^-1 = a^(254) = exp[255 - log(a)]
inline std::uint8_t gf_inv(std::uint8_t a) {
    return gf().exp[255 - gf().log[a]];
}

// 编码系数（Vandermonde）：第 parity_index 个校验分片对第 data_index 个
// 数据分片的系数。i=0 时恒为 1，即第 0 个校验分片就是全体 XOR。
inline std::uint8_t coef(std::size_t parity_index, std::size_t data_index) {
    return gf_pow(static_cast<std::uint8_t>(data_index + 1),
                  static_cast<int>(parity_index));
}

// dst ^= c * src（逐字节 GF 运算；src 为指针/长度，不足 width 的部分按零
// 处理——零与任何域元素相乘仍为零，等价于补齐后再编码）
void gf_mul_add(Bytes& dst, const std::uint8_t* src, std::size_t src_len,
                std::uint8_t c, std::size_t width) {
    if (c == 0 || src_len == 0) return;
    std::size_t n = std::min(dst.size(), std::min(src_len, width));
    if (c == 1) {   // 系数为 1 时就是纯 XOR，走快路径
        for (std::size_t i = 0; i < n; ++i) dst[i] ^= src[i];
        return;
    }
    auto lc = gf().log[c];
    for (std::size_t i = 0; i < n; ++i) {
        if (src[i] != 0) dst[i] ^= gf().exp[lc + gf().log[src[i]]];
    }
}

void gf_mul_add(Bytes& dst, const Bytes& src, std::uint8_t c, std::size_t width) {
    gf_mul_add(dst, src.data(), src.size(), c, width);
}

// dst ^= c * src 的原始指针版（dst 定长 width，调用方持有内存）
void gf_mul_add_raw(std::uint8_t* dst, const std::uint8_t* src,
                    std::size_t src_len, std::uint8_t c, std::size_t width) {
    if (c == 0 || src_len == 0) return;
    std::size_t n = std::min(src_len, width);
    if (c == 1) {
        for (std::size_t i = 0; i < n; ++i) dst[i] ^= src[i];
        return;
    }
    auto lc = gf().log[c];
    for (std::size_t i = 0; i < n; ++i) {
        if (src[i] != 0) dst[i] ^= gf().exp[lc + gf().log[src[i]]];
    }
}

} // namespace

namespace tight_detail {

void rs_encode_into_raw(const ReedSolomon::Span* fragments,
                        std::size_t fragment_count,
                        std::size_t parity_count, std::size_t width,
                        std::uint8_t* out) {
    for (std::size_t p = 0; p < parity_count; ++p) {
        std::uint8_t* dst = out + p * width;
        std::memset(dst, 0, width);
        for (std::size_t j = 0; j < fragment_count; ++j) {
            gf_mul_add_raw(dst, fragments[j].data, fragments[j].size,
                           coef(p, j), width);
        }
    }
}

} // namespace tight_detail

std::vector<Bytes> ReedSolomon::encode(const std::vector<Bytes>& data,
                                       std::size_t parity_count, std::size_t width) {
    std::vector<Span> spans;
    spans.reserve(data.size());
    for (const auto& f : data) spans.push_back({f.data(), f.size()});
    return encode(spans, parity_count, width);
}

std::vector<Bytes> ReedSolomon::encode(const std::vector<Span>& fragments,
                                       std::size_t parity_count, std::size_t width) {
    std::vector<Bytes> parities;
    encode_into(fragments, parity_count, width, parities);
    return parities;
}

void ReedSolomon::encode_into(const std::vector<Span>& fragments,
                              std::size_t parity_count, std::size_t width,
                              std::vector<Bytes>& out) {
    // 复用调用方缓冲：仅容量不足时扩容，热点路径摊销零堆分配
    if (out.size() < parity_count) out.resize(parity_count);
    for (std::size_t p = 0; p < parity_count; ++p) {
        if (out[p].size() < width) {
            out[p].assign(width, 0);
        } else {
            std::fill(out[p].begin(), out[p].begin() + width, 0);
        }
        for (std::size_t j = 0; j < fragments.size(); ++j) {
            gf_mul_add(out[p], fragments[j].data, fragments[j].size,
                       coef(p, j), width);
        }
    }
}

bool ReedSolomon::decode(std::vector<std::optional<Bytes>>& data,
                         const std::vector<std::pair<std::size_t, Bytes>>& parity,
                         std::size_t width) {
    // 收集缺失的数据分片索引
    std::vector<std::size_t> missing;
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (!data[i].has_value()) missing.push_back(i);
    }
    if (missing.empty()) return true;
    if (missing.size() > parity.size()) return false;   // 校验不足，无法恢复

    std::size_t m = missing.size();

    // 为每个用到的校验分片计算伴随式：
    //   syn[r] = parity_r ⊕ Σ(现存数据分片 j) coef(p_r, j) * data_j
    // 剩下 syn[r] = Σ(缺失分片 c) coef(p_r, missing[c]) * data_{missing[c]}
    // 即线性方程组 A · x = syn，A[r][c] = coef(p_r, missing[c])。
    std::vector<std::vector<std::uint8_t>> a(m, std::vector<std::uint8_t>(m));
    std::vector<Bytes> syn(m);
    for (std::size_t r = 0; r < m; ++r) {
        std::size_t pi = parity[r].first;
        syn[r] = parity[r].second;
        syn[r].resize(width, 0);
        for (std::size_t j = 0; j < data.size(); ++j) {
            if (data[j].has_value()) {
                gf_mul_add(syn[r], *data[j], coef(pi, j), width);
            }
        }
        for (std::size_t c = 0; c < m; ++c) {
            a[r][c] = coef(pi, missing[c]);
        }
    }

    // GF(2^8) 高斯消元（Vandermonde 任意方子矩阵可逆，不会奇异）
    for (std::size_t col = 0; col < m; ++col) {
        std::size_t pivot = col;
        while (pivot < m && a[pivot][col] == 0) ++pivot;
        if (pivot == m) return false;
        if (pivot != col) {
            std::swap(a[pivot], a[col]);
            std::swap(syn[pivot], syn[col]);
        }
        // 主行归一化（主元化为 1）
        std::uint8_t inv = gf_inv(a[col][col]);
        for (std::size_t j = col; j < m; ++j) a[col][j] = gf_mul(a[col][j], inv);
        for (auto& b : syn[col]) b = gf_mul(b, inv);
        // 消去其它行的本列
        for (std::size_t r = 0; r < m; ++r) {
            if (r == col) continue;
            std::uint8_t f = a[r][col];
            if (f == 0) continue;
            for (std::size_t j = col; j < m; ++j) a[r][j] ^= gf_mul(f, a[col][j]);
            gf_mul_add(syn[r], syn[col], f, width);
        }
    }

    // 消元后 A = I，syn[c] 即缺失分片 missing[c] 的完整内容，回填
    for (std::size_t c = 0; c < m; ++c) {
        data[missing[c]] = std::move(syn[c]);
    }
    return true;
}

} // namespace tight
