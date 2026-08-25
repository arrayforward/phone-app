#include "crypto/crypto.hpp"

#include <cstring>
#include <random>

namespace tight::tight_detail {

// ============================================================
// SHA-256（FIPS 180-4）
// ============================================================

namespace {

const std::uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline std::uint32_t rotr32(std::uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

struct Sha256Ctx {
    std::uint32_t h[8];
    std::uint8_t  buf[64];
    std::size_t   buf_len{0};
    std::uint64_t total_len{0};

    Sha256Ctx()
        : h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

    void compress(const std::uint8_t* block) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                   (static_cast<std::uint32_t>(block[i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            std::uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3],
                      e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            std::uint32_t ch = (e & f) ^ (~e & g);
            std::uint32_t t1 = hh + s1 + ch + kSha256K[i] + w[i];
            std::uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t t2 = s0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(const std::uint8_t* data, std::size_t len) {
        total_len += len;
        while (len > 0) {
            std::size_t take = 64 - buf_len;
            if (take > len) take = len;
            std::memcpy(buf + buf_len, data, take);
            buf_len += take;
            data += take;
            len -= take;
            if (buf_len == 64) {
                compress(buf);
                buf_len = 0;
            }
        }
    }

    void finish(std::uint8_t* out32) {
        std::uint64_t bit_len = total_len * 8;
        std::uint8_t pad = 0x80;
        update(&pad, 1);
        std::uint8_t zero = 0;
        while (buf_len != 56) update(&zero, 1);
        std::uint8_t len_be[8];
        for (int i = 0; i < 8; ++i) {
            len_be[i] = static_cast<std::uint8_t>((bit_len >> (56 - i * 8)) & 0xFF);
        }
        update(len_be, 8);
        for (int i = 0; i < 8; ++i) {
            out32[i * 4]     = static_cast<std::uint8_t>((h[i] >> 24) & 0xFF);
            out32[i * 4 + 1] = static_cast<std::uint8_t>((h[i] >> 16) & 0xFF);
            out32[i * 4 + 2] = static_cast<std::uint8_t>((h[i] >> 8) & 0xFF);
            out32[i * 4 + 3] = static_cast<std::uint8_t>(h[i] & 0xFF);
        }
    }
};

void hmac_sha256(const std::uint8_t* key, std::size_t key_len,
                 const std::uint8_t* msg, std::size_t msg_len,
                 std::uint8_t* out32) {
    std::uint8_t k0[64] = {0};
    if (key_len > 64) {
        auto kh = sha256(key, key_len);
        std::memcpy(k0, kh.data(), 32);
    } else {
        std::memcpy(k0, key, key_len);
    }
    std::uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = static_cast<std::uint8_t>(k0[i] ^ 0x36);
        opad[i] = static_cast<std::uint8_t>(k0[i] ^ 0x5C);
    }
    Sha256Ctx inner;
    inner.update(ipad, 64);
    inner.update(msg, msg_len);
    std::uint8_t inner_out[32];
    inner.finish(inner_out);
    Sha256Ctx outer;
    outer.update(opad, 64);
    outer.update(inner_out, 32);
    outer.finish(out32);
}

} // namespace

std::array<std::uint8_t, 32> sha256(const std::uint8_t* data, std::size_t len) {
    Sha256Ctx ctx;
    ctx.update(data, len);
    std::array<std::uint8_t, 32> out{};
    ctx.finish(out.data());
    return out;
}

std::array<std::uint8_t, 32> hkdf_sha256(const std::uint8_t* ikm, std::size_t ikm_len,
                                         const std::uint8_t* salt, std::size_t salt_len,
                                         const std::string& info) {
    // extract：PRK = HMAC(salt, IKM)
    std::uint8_t zero_salt[32] = {0};
    if (!salt || salt_len == 0) {
        salt = zero_salt;
        salt_len = 32;
    }
    std::uint8_t prk[32];
    hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
    // expand：32 字节输出只需一块 T(1) = HMAC(PRK, info || 0x01)
    std::array<std::uint8_t, 32> out{};
    std::string msg = info;
    msg.push_back('\x01');
    hmac_sha256(prk, 32, reinterpret_cast<const std::uint8_t*>(msg.data()),
                msg.size(), out.data());
    return out;
}

// ============================================================
// X25519（RFC 7748，5×51 位 limb + __int128）
// ============================================================

namespace {

using Fe = std::array<std::uint64_t, 5>;      // GF(2^255-19) 域元素
constexpr std::uint64_t kMask51 = (1ULL << 51) - 1;

std::uint64_t load_le64(const std::uint8_t* b) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(b[i]) << (i * 8);
    return v;
}

Fe fe_frombytes(const std::uint8_t* s) {
    return {
        load_le64(s) & kMask51,
        (load_le64(s + 6) >> 3) & kMask51,
        (load_le64(s + 12) >> 6) & kMask51,
        (load_le64(s + 19) >> 1) & kMask51,
        (load_le64(s + 24) >> 12) & kMask51,   // 忽略最高位 bit255
    };
}

void fe_tobytes(std::uint8_t* s, const Fe& in) {
    Fe h = in;
    // 三轮进位传播，保证 h < 2p
    for (int pass = 0; pass < 3; ++pass) {
        std::uint64_t c = h[0] >> 51; h[0] &= kMask51; h[1] += c;
        c = h[1] >> 51; h[1] &= kMask51; h[2] += c;
        c = h[2] >> 51; h[2] &= kMask51; h[3] += c;
        c = h[3] >> 51; h[3] &= kMask51; h[4] += c;
        c = h[4] >> 51; h[4] &= kMask51; h[0] += 19 * c;
    }
    // 条件减 p（h < 2p）：计算 h - p，无借位则采用差值
    const std::uint64_t p[5] = {kMask51 - 18, kMask51, kMask51, kMask51, kMask51};
    Fe m;
    std::int64_t borrow = 0;
    for (int i = 0; i < 5; ++i) {
        std::int64_t t = static_cast<std::int64_t>(h[i]) -
                         static_cast<std::int64_t>(p[i]) - borrow;
        borrow = (t < 0) ? 1 : 0;
        m[i] = static_cast<std::uint64_t>(t) & kMask51;
    }
    // borrow==1 表示 h < p，保留 h；否则采用 h - p
    std::uint64_t keep = static_cast<std::uint64_t>(-borrow);
    Fe out;
    for (int i = 0; i < 5; ++i) {
        out[i] = (keep & h[i]) | (~keep & m[i]);
    }
    // 51 位 limb 打包为 32 字节小端
    s[0]  = static_cast<std::uint8_t>(out[0]);
    s[1]  = static_cast<std::uint8_t>(out[0] >> 8);
    s[2]  = static_cast<std::uint8_t>(out[0] >> 16);
    s[3]  = static_cast<std::uint8_t>(out[0] >> 24);
    s[4]  = static_cast<std::uint8_t>(out[0] >> 32);
    s[5]  = static_cast<std::uint8_t>(out[0] >> 40);
    s[6]  = static_cast<std::uint8_t>((out[0] >> 48) | (out[1] << 3));
    s[7]  = static_cast<std::uint8_t>(out[1] >> 5);
    s[8]  = static_cast<std::uint8_t>(out[1] >> 13);
    s[9]  = static_cast<std::uint8_t>(out[1] >> 21);
    s[10] = static_cast<std::uint8_t>(out[1] >> 29);
    s[11] = static_cast<std::uint8_t>(out[1] >> 37);
    s[12] = static_cast<std::uint8_t>((out[1] >> 45) | (out[2] << 6));
    s[13] = static_cast<std::uint8_t>(out[2] >> 2);
    s[14] = static_cast<std::uint8_t>(out[2] >> 10);
    s[15] = static_cast<std::uint8_t>(out[2] >> 18);
    s[16] = static_cast<std::uint8_t>(out[2] >> 26);
    s[17] = static_cast<std::uint8_t>(out[2] >> 34);
    s[18] = static_cast<std::uint8_t>(out[2] >> 42);
    s[19] = static_cast<std::uint8_t>((out[2] >> 50) | (out[3] << 1));
    s[20] = static_cast<std::uint8_t>(out[3] >> 7);
    s[21] = static_cast<std::uint8_t>(out[3] >> 15);
    s[22] = static_cast<std::uint8_t>(out[3] >> 23);
    s[23] = static_cast<std::uint8_t>(out[3] >> 31);
    s[24] = static_cast<std::uint8_t>(out[3] >> 39);
    s[25] = static_cast<std::uint8_t>((out[3] >> 47) | (out[4] << 4));
    s[26] = static_cast<std::uint8_t>(out[4] >> 4);
    s[27] = static_cast<std::uint8_t>(out[4] >> 12);
    s[28] = static_cast<std::uint8_t>(out[4] >> 20);
    s[29] = static_cast<std::uint8_t>(out[4] >> 28);
    s[30] = static_cast<std::uint8_t>(out[4] >> 36);
    s[31] = static_cast<std::uint8_t>(out[4] >> 44);
}

Fe fe_add(const Fe& f, const Fe& g) {
    return {f[0] + g[0], f[1] + g[1], f[2] + g[2], f[3] + g[3], f[4] + g[4]};
}

Fe fe_sub(const Fe& f, const Fe& g) {
    // 加 2p 再减，避免下溢
    return {
        f[0] + 0xFFFFFFFFFFFDAULL - g[0],   // 2*(2^51-19)
        f[1] + 0xFFFFFFFFFFFFEULL - g[1],   // 2*(2^51-1)
        f[2] + 0xFFFFFFFFFFFFEULL - g[2],
        f[3] + 0xFFFFFFFFFFFFEULL - g[3],
        f[4] + 0xFFFFFFFFFFFFEULL - g[4],
    };
}

Fe fe_mul(const Fe& f, const Fe& g) {
    using u128 = unsigned __int128;
    u128 f0 = f[0], f1 = f[1], f2 = f[2], f3 = f[3], f4 = f[4];
    u128 g0 = g[0], g1 = g[1], g2 = g[2], g3 = g[3], g4 = g[4];
    // 2^255 ≡ 19 (mod p)，高位折叠
    u128 r0 = f0 * g0 + 19 * (f1 * g4 + f2 * g3 + f3 * g2 + f4 * g1);
    u128 r1 = f0 * g1 + f1 * g0 + 19 * (f2 * g4 + f3 * g3 + f4 * g2);
    u128 r2 = f0 * g2 + f1 * g1 + f2 * g0 + 19 * (f3 * g4 + f4 * g3);
    u128 r3 = f0 * g3 + f1 * g2 + f2 * g1 + f3 * g0 + 19 * (f4 * g4);
    u128 r4 = f0 * g4 + f1 * g3 + f2 * g2 + f3 * g1 + f4 * g0;
    std::uint64_t c;
    c = static_cast<std::uint64_t>(r0 >> 51); r0 &= kMask51; r1 += c;
    c = static_cast<std::uint64_t>(r1 >> 51); r1 &= kMask51; r2 += c;
    c = static_cast<std::uint64_t>(r2 >> 51); r2 &= kMask51; r3 += c;
    c = static_cast<std::uint64_t>(r3 >> 51); r3 &= kMask51; r4 += c;
    c = static_cast<std::uint64_t>(r4 >> 51); r4 &= kMask51; r0 += 19 * c;
    c = static_cast<std::uint64_t>(r0 >> 51); r0 &= kMask51; r1 += c;
    return {static_cast<std::uint64_t>(r0), static_cast<std::uint64_t>(r1),
            static_cast<std::uint64_t>(r2), static_cast<std::uint64_t>(r3),
            static_cast<std::uint64_t>(r4)};
}

Fe fe_sq(const Fe& f) { return fe_mul(f, f); }

// 域元素乘小标量（带完整进位约化，保证输出 limb < 2^51）
Fe fe_mul_scalar(const Fe& f, std::uint64_t s) {
    using u128 = unsigned __int128;
    u128 r0 = static_cast<u128>(f[0]) * s;
    u128 r1 = static_cast<u128>(f[1]) * s;
    u128 r2 = static_cast<u128>(f[2]) * s;
    u128 r3 = static_cast<u128>(f[3]) * s;
    u128 r4 = static_cast<u128>(f[4]) * s;
    std::uint64_t c;
    c = static_cast<std::uint64_t>(r0 >> 51); r0 &= kMask51; r1 += c;
    c = static_cast<std::uint64_t>(r1 >> 51); r1 &= kMask51; r2 += c;
    c = static_cast<std::uint64_t>(r2 >> 51); r2 &= kMask51; r3 += c;
    c = static_cast<std::uint64_t>(r3 >> 51); r3 &= kMask51; r4 += c;
    c = static_cast<std::uint64_t>(r4 >> 51); r4 &= kMask51; r0 += 19 * c;
    c = static_cast<std::uint64_t>(r0 >> 51); r0 &= kMask51; r1 += c;
    return {static_cast<std::uint64_t>(r0), static_cast<std::uint64_t>(r1),
            static_cast<std::uint64_t>(r2), static_cast<std::uint64_t>(r3),
            static_cast<std::uint64_t>(r4)};
}

Fe fe_sq_n(Fe f, int n) {
    for (int i = 0; i < n; ++i) f = fe_sq(f);
    return f;
}

// 域求逆：z^(p-2) = z^(2^255-21)，标准加法链：
// z^2 → z^9 → z^11 → z^22 → z^(2^5-1) → z^(2^10-1) → … → z^(2^250-1)
// → 平方 5 次得 z^(2^255-32)，最后乘 z^11 得 z^(2^255-21)。
Fe fe_inv(const Fe& z) {
    Fe z2 = fe_sq(z);
    Fe z8 = fe_sq_n(z2, 2);
    Fe z9 = fe_mul(z, z8);
    Fe z11 = fe_mul(z2, z9);
    Fe z22 = fe_sq(z11);
    Fe z_5_0 = fe_mul(z9, z22);                    // z^(2^5-1) = z^31
    Fe z_10_0 = fe_mul(fe_sq_n(z_5_0, 5), z_5_0);
    Fe z_20_0 = fe_mul(fe_sq_n(z_10_0, 10), z_10_0);
    Fe z_40_0 = fe_mul(fe_sq_n(z_20_0, 20), z_20_0);
    Fe z_50_0 = fe_mul(fe_sq_n(z_40_0, 10), z_10_0);
    Fe z_100_0 = fe_mul(fe_sq_n(z_50_0, 50), z_50_0);
    Fe z_200_0 = fe_mul(fe_sq_n(z_100_0, 100), z_100_0);
    Fe z_250_0 = fe_mul(fe_sq_n(z_200_0, 50), z_50_0);
    return fe_mul(fe_sq_n(z_250_0, 5), z11);
}

void fe_cswap(Fe& f, Fe& g, std::uint64_t swap) {
    std::uint64_t mask = 0 - swap;   // swap=1 → 全 1
    for (int i = 0; i < 5; ++i) {
        std::uint64_t t = mask & (f[i] ^ g[i]);
        f[i] ^= t;
        g[i] ^= t;
    }
}

} // namespace

X25519KeyPair x25519_generate() {
    X25519KeyPair kp;
    std::random_device rd;
    for (auto& b : kp.private_key) {
        b = static_cast<std::uint8_t>(rd());
    }
    // 私钥 clamp 在 x25519() 内进行；公钥 = X25519(私钥, 基点 9)
    std::array<std::uint8_t, 32> base{};
    base[0] = 9;
    x25519(kp.public_key, kp.private_key, base);
    return kp;
}

bool x25519(std::array<std::uint8_t, 32>& out,
            const std::array<std::uint8_t, 32>& private_key,
            const std::array<std::uint8_t, 32>& peer_public_key) {
    // 标量 clamp
    std::array<std::uint8_t, 32> e = private_key;
    e[0] &= 248;
    e[31] &= 127;
    e[31] |= 64;

    Fe x1 = fe_frombytes(peer_public_key.data());
    Fe x2 = {1, 0, 0, 0, 0};
    Fe z2 = {0, 0, 0, 0, 0};
    Fe x3 = x1;
    Fe z3 = {1, 0, 0, 0, 0};
    std::uint64_t swap = 0;

    // Montgomery 阶梯（a24 = 121665）
    for (int t = 254; t >= 0; --t) {
        std::uint64_t kt = (e[t / 8] >> (t % 8)) & 1;
        swap ^= kt;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = kt;

        Fe a = fe_add(x2, z2);
        Fe aa = fe_sq(a);
        Fe b = fe_sub(x2, z2);
        Fe bb = fe_sq(b);
        Fe e2 = fe_sub(aa, bb);
        Fe c = fe_add(x3, z3);
        Fe d = fe_sub(x3, z3);
        Fe da = fe_mul(d, a);
        Fe cb = fe_mul(c, b);
        Fe da_cb = fe_add(da, cb);
        Fe da_cb2 = fe_sub(da, cb);
        x3 = fe_sq(da_cb);
        z3 = fe_mul(x1, fe_sq(da_cb2));
        x2 = fe_mul(aa, bb);
        // z2 = E * (AA + a24 * E)，标量乘后必须进位约化再相加
        z2 = fe_mul(e2, fe_add(aa, fe_mul_scalar(e2, 121665)));
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    Fe result = fe_mul(x2, fe_inv(z2));
    fe_tobytes(out.data(), result);

    // 低阶点检查：结果全零则共享秘密无效
    std::uint8_t acc = 0;
    for (auto b : out) acc |= b;
    return acc != 0;
}

// ============================================================
// AES-256（FIPS 197，S 盒运行时生成：GF(2^8)/0x11B 逆元 + 仿射变换）
// ============================================================

namespace {

std::uint8_t aes_mul(std::uint8_t a, std::uint8_t b) {
    std::uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = static_cast<std::uint8_t>((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
        b >>= 1;
    }
    return r;
}

std::uint8_t aes_inv(std::uint8_t a) {
    if (a == 0) return 0;
    std::uint8_t result = 1, base = a;
    int e = 254;   // a^(2^8-2) = a^254 为乘法逆元
    while (e) {
        if (e & 1) result = aes_mul(result, base);
        base = aes_mul(base, base);
        e >>= 1;
    }
    return result;
}

struct AesTables {
    std::array<std::uint8_t, 256> sbox{};
    AesTables() {
        for (int x = 0; x < 256; ++x) {
            std::uint8_t y = aes_inv(static_cast<std::uint8_t>(x));
            auto rotl = [&](int n) {
                return static_cast<std::uint8_t>((y << n) | (y >> (8 - n)));
            };
            sbox[x] = static_cast<std::uint8_t>(0x63 ^ y ^ rotl(1) ^ rotl(2) ^
                                                rotl(3) ^ rotl(4));
        }
    }
};

const AesTables& aes_tables() {
    static AesTables t;
    return t;
}

struct Aes256 {
    std::uint8_t rk[14 + 1][16];   // 15 个轮密钥（14 轮）

    explicit Aes256(const std::array<std::uint8_t, 32>& key) {
        const auto& sbox = aes_tables().sbox;
        std::uint32_t w[60];
        for (int i = 0; i < 8; ++i) {
            w[i] = (static_cast<std::uint32_t>(key[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(key[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(key[i * 4 + 2]) << 8) |
                   (static_cast<std::uint32_t>(key[i * 4 + 3]));
        }
        std::uint32_t rcon = 0x01000000;
        for (int i = 8; i < 60; ++i) {
            std::uint32_t t = w[i - 1];
            if (i % 8 == 0) {
                t = (static_cast<std::uint32_t>(sbox[(t >> 16) & 0xFF]) << 24) |
                    (static_cast<std::uint32_t>(sbox[(t >> 8) & 0xFF]) << 16) |
                    (static_cast<std::uint32_t>(sbox[t & 0xFF]) << 8) |
                    (static_cast<std::uint32_t>(sbox[(t >> 24) & 0xFF]));
                t ^= rcon;
                rcon = static_cast<std::uint32_t>(aes_mul(
                             static_cast<std::uint8_t>(rcon >> 24), 2)) << 24;
            } else if (i % 8 == 4) {
                t = (static_cast<std::uint32_t>(sbox[(t >> 24) & 0xFF]) << 24) |
                    (static_cast<std::uint32_t>(sbox[(t >> 16) & 0xFF]) << 16) |
                    (static_cast<std::uint32_t>(sbox[(t >> 8) & 0xFF]) << 8) |
                    (static_cast<std::uint32_t>(sbox[t & 0xFF]));
            }
            w[i] = w[i - 8] ^ t;
        }
        for (int r = 0; r < 15; ++r) {
            for (int c = 0; c < 4; ++c) {
                std::uint32_t word = w[r * 4 + c];
                rk[r][c * 4]     = static_cast<std::uint8_t>((word >> 24) & 0xFF);
                rk[r][c * 4 + 1] = static_cast<std::uint8_t>((word >> 16) & 0xFF);
                rk[r][c * 4 + 2] = static_cast<std::uint8_t>((word >> 8) & 0xFF);
                rk[r][c * 4 + 3] = static_cast<std::uint8_t>(word & 0xFF);
            }
        }
    }

    void encrypt_block(const std::uint8_t* in, std::uint8_t* out) const {
        const auto& sbox = aes_tables().sbox;
        std::uint8_t s[16];
        for (int i = 0; i < 16; ++i) s[i] = static_cast<std::uint8_t>(in[i] ^ rk[0][i]);

        auto sub_shift = [&]() {
            std::uint8_t t[16];
            // SubBytes + ShiftRows（状态按列主序：s[row + 4*col]）
            t[0] = sbox[s[0]];   t[4] = sbox[s[4]];   t[8]  = sbox[s[8]];   t[12] = sbox[s[12]];
            t[1] = sbox[s[5]];   t[5] = sbox[s[9]];   t[9]  = sbox[s[13]];  t[13] = sbox[s[1]];
            t[2] = sbox[s[10]];  t[6] = sbox[s[14]];  t[10] = sbox[s[2]];   t[14] = sbox[s[6]];
            t[3] = sbox[s[15]];  t[7] = sbox[s[3]];   t[11] = sbox[s[7]];   t[15] = sbox[s[11]];
            std::memcpy(s, t, 16);
        };
        auto mix_columns = [&]() {
            for (int c = 0; c < 4; ++c) {
                std::uint8_t* col = s + c * 4;
                std::uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = static_cast<std::uint8_t>(aes_mul(a0, 2) ^ aes_mul(a1, 3) ^ a2 ^ a3);
                col[1] = static_cast<std::uint8_t>(a0 ^ aes_mul(a1, 2) ^ aes_mul(a2, 3) ^ a3);
                col[2] = static_cast<std::uint8_t>(a0 ^ a1 ^ aes_mul(a2, 2) ^ aes_mul(a3, 3));
                col[3] = static_cast<std::uint8_t>(aes_mul(a0, 3) ^ a1 ^ a2 ^ aes_mul(a3, 2));
            }
        };

        for (int round = 1; round < 14; ++round) {
            sub_shift();
            mix_columns();
            for (int i = 0; i < 16; ++i) s[i] ^= rk[round][i];
        }
        sub_shift();   // 最后一轮无 MixColumns
        for (int i = 0; i < 16; ++i) out[i] = static_cast<std::uint8_t>(s[i] ^ rk[14][i]);
    }
};

// ---------- GCM（NIST SP 800-38D） ----------

// GHASH 域乘法（GF(2^128)，约化多项式 x^128 + x^7 + x^2 + x + 1）
void ghash_mul(std::uint8_t* x, const std::uint8_t* y) {
    std::uint8_t z[16] = {0};
    std::uint8_t v[16];
    std::memcpy(v, y, 16);
    for (int i = 0; i < 128; ++i) {
        if ((x[i / 8] >> (7 - (i % 8))) & 1) {
            for (int j = 0; j < 16; ++j) z[j] ^= v[j];
        }
        bool lsb = v[15] & 1;
        for (int j = 15; j > 0; --j) {
            v[j] = static_cast<std::uint8_t>((v[j] >> 1) | (v[j - 1] << 7));
        }
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xE1;
    }
    std::memcpy(x, z, 16);
}

void ghash_update(std::uint8_t* x, const std::uint8_t* h,
                  const std::uint8_t* data, std::size_t len) {
    std::uint8_t block[16];
    while (len > 0) {
        std::size_t take = len < 16 ? len : 16;
        std::memset(block, 0, 16);
        std::memcpy(block, data, take);
        for (int i = 0; i < 16; ++i) x[i] ^= block[i];
        ghash_mul(x, h);
        data += take;
        len -= take;
    }
}

void gcm_compute_tag(const Aes256& aes, const std::uint8_t* j0,
                     const std::uint8_t* aad, std::size_t aad_len,
                     const std::uint8_t* ct, std::size_t ct_len,
                     std::uint8_t* tag) {
    std::uint8_t h[16];
    std::uint8_t zero[16] = {0};
    aes.encrypt_block(zero, h);

    std::uint8_t x[16] = {0};
    ghash_update(x, h, aad, aad_len);
    ghash_update(x, h, ct, ct_len);
    std::uint8_t len_block[16] = {0};
    std::uint64_t aad_bits = static_cast<std::uint64_t>(aad_len) * 8;
    std::uint64_t ct_bits = static_cast<std::uint64_t>(ct_len) * 8;
    for (int i = 0; i < 8; ++i) {
        len_block[i] = static_cast<std::uint8_t>((aad_bits >> (56 - i * 8)) & 0xFF);
        len_block[8 + i] = static_cast<std::uint8_t>((ct_bits >> (56 - i * 8)) & 0xFF);
    }
    ghash_update(x, h, len_block, 16);

    std::uint8_t ej0[16];
    aes.encrypt_block(j0, ej0);
    for (int i = 0; i < 16; ++i) tag[i] = static_cast<std::uint8_t>(x[i] ^ ej0[i]);
}

void aes256_gcm_crypt(const Aes256& aes, const std::uint8_t* j0,
                      const std::uint8_t* in, std::size_t len, std::uint8_t* out) {
    std::uint8_t ctr[16];
    std::memcpy(ctr, j0, 16);
    std::uint8_t ks[16];
    while (len > 0) {
        // 计数器后 4 字节大端自增（首块使用 inc32(J0)）
        for (int i = 15; i >= 12; --i) {
            if (++ctr[i] != 0) break;
        }
        aes.encrypt_block(ctr, ks);
        std::size_t take = len < 16 ? len : 16;
        for (std::size_t i = 0; i < take; ++i) out[i] = static_cast<std::uint8_t>(in[i] ^ ks[i]);
        in += take;
        out += take;
        len -= take;
    }
}

} // namespace

bool aes256_gcm_encrypt(const std::array<std::uint8_t, 32>& key,
                        const std::array<std::uint8_t, kGcmNonceSize>& nonce,
                        const std::uint8_t* aad, std::size_t aad_len,
                        const std::uint8_t* plaintext, std::size_t pt_len,
                        std::uint8_t* ciphertext,
                        std::uint8_t* tag) {
    Aes256 aes(key);
    // 96 位 nonce：J0 = nonce || 0x00000001
    std::uint8_t j0[16] = {0};
    std::memcpy(j0, nonce.data(), kGcmNonceSize);
    j0[15] = 1;
    aes256_gcm_crypt(aes, j0, plaintext, pt_len, ciphertext);
    gcm_compute_tag(aes, j0, aad, aad_len, ciphertext, pt_len, tag);
    return true;
}

bool aes256_gcm_decrypt(const std::array<std::uint8_t, 32>& key,
                        const std::array<std::uint8_t, kGcmNonceSize>& nonce,
                        const std::uint8_t* aad, std::size_t aad_len,
                        const std::uint8_t* ciphertext, std::size_t ct_len,
                        const std::uint8_t* tag,
                        std::uint8_t* plaintext) {
    Aes256 aes(key);
    std::uint8_t j0[16] = {0};
    std::memcpy(j0, nonce.data(), kGcmNonceSize);
    j0[15] = 1;
    std::uint8_t expect[16];
    gcm_compute_tag(aes, j0, aad, aad_len, ciphertext, ct_len, expect);
    // 常数时间比较认证标签
    std::uint8_t diff = 0;
    for (int i = 0; i < 16; ++i) diff |= static_cast<std::uint8_t>(expect[i] ^ tag[i]);
    if (diff != 0) return false;
    aes256_gcm_crypt(aes, j0, ciphertext, ct_len, plaintext);
    return true;
}

} // namespace tight::tight_detail
