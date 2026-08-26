// host 端单测：增强层熵编码 + 残差编解码（协议 docs/protocol.md §3.4）。
// 构建：g++ -O2 -std=c++17 shared/layered/entropy.cpp shared/layered/residual.cpp
//       shared/layered/layered_test.cpp -o layered_test
#include "entropy.h"
#include "residual.h"
#include "camouflage.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            std::printf(__VA_ARGS__);                          \
            std::printf("\n");                                 \
            ++g_failures;                                      \
        }                                                      \
    } while (0)

// 合成重建帧：orig + 有界噪声（大量 0，部分 ±1~2，偶发大残差），模拟 H.264
// 低码率重建误差分布（经验：~85% 零、±1~2 为主、边缘偶发大误差）。
// |噪声| ≤ 64 保证 clamp 不触发 → 合成逐字节还原 orig。
std::vector<std::uint8_t> make_noisy_recon(const std::vector<std::uint8_t>& orig,
                                           std::mt19937& rng, double zero_ratio) {
    std::vector<std::uint8_t> recon(orig.size());
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    std::uniform_int_distribution<int> small(-2, 2);
    std::uniform_int_distribution<int> big(-64, 64);
    // 非零部分里 3% 为大残差
    const double big_thr = zero_ratio + (1.0 - zero_ratio) * 0.97;
    for (std::size_t i = 0; i < orig.size(); ++i) {
        double r = u01(rng);
        int d = 0;
        if (r > zero_ratio) d = (r > big_thr) ? big(rng) : small(rng);
        int v = (int)orig[i] + d;
        recon[i] = (std::uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    return recon;
}

// recon 紧凑 planar → 带 stride/pixelStride 的平面视图（模拟 MediaCodec Image）
struct StridedRecon {
    std::vector<std::uint8_t> y, u, v;   // 各自 stride*rows 布局
    int y_stride, y_ps, u_stride, u_ps, v_stride, v_ps;
    // semi_planar=true 时 U/V 合并为 NV12 交错缓冲（u/v 指向同一缓冲错开 1 字节）
    std::vector<std::uint8_t> uv;
    bool semi_planar;
};

StridedRecon make_strided(const std::vector<std::uint8_t>& recon, int w, int h,
                          bool semi_planar) {
    StridedRecon s;
    s.y_stride = w + 16;  // 模拟非紧凑行距
    s.y_ps = 1;
    s.y.assign((std::size_t)s.y_stride * h, 0xAB);
    for (int r = 0; r < h; ++r)
        std::memcpy(s.y.data() + (std::size_t)r * s.y_stride,
                    recon.data() + (std::size_t)r * w, w);
    const int cw = w / 2, ch = h / 2;
    const std::uint8_t* up = recon.data() + (std::size_t)w * h;
    const std::uint8_t* vp = up + (std::size_t)cw * ch;
    s.semi_planar = semi_planar;
    if (semi_planar) {
        s.u_stride = cw * 2 + 8;
        s.v_stride = s.u_stride;
        s.u_ps = s.v_ps = 2;
        s.uv.assign((std::size_t)s.u_stride * ch + 1, 0xCD);
        for (int r = 0; r < ch; ++r)
            for (int c = 0; c < cw; ++c) {
                s.uv[(std::size_t)r * s.u_stride + c * 2] = up[(std::size_t)r * cw + c];
                s.uv[(std::size_t)r * s.u_stride + c * 2 + 1] = vp[(std::size_t)r * cw + c];
            }
    } else {
        s.u_stride = cw + 8;
        s.v_stride = cw + 4;
        s.u_ps = s.v_ps = 1;
        s.u.assign((std::size_t)s.u_stride * ch, 0xCD);
        s.v.assign((std::size_t)s.v_stride * ch, 0xCD);
        for (int r = 0; r < ch; ++r) {
            std::memcpy(s.u.data() + (std::size_t)r * s.u_stride, up + (std::size_t)r * cw, cw);
            std::memcpy(s.v.data() + (std::size_t)r * s.v_stride, vp + (std::size_t)r * cw, cw);
        }
    }
    return s;
}

// 核心往返：orig → (compute_residual w/ strided recon) → encode → decode → sym 一致；
// apply_residual(recon, sym) == orig
void test_roundtrip(int w, int h, bool semi_planar, double zero_ratio,
                    std::mt19937& rng, const char* what) {
    std::vector<std::uint8_t> orig(layered::frame_bytes(w, h));
    std::uniform_int_distribution<int> byte(0, 255);
    for (auto& b : orig) b = (std::uint8_t)byte(rng);
    std::vector<std::uint8_t> recon = make_noisy_recon(orig, rng, zero_ratio);
    StridedRecon sr = make_strided(recon, w, h, semi_planar);

    std::vector<std::uint8_t> sym(layered::frame_bytes(w, h), 0);
    const std::uint8_t* ru = semi_planar ? sr.uv.data() : sr.u.data();
    const std::uint8_t* rv = semi_planar ? sr.uv.data() + 1 : sr.v.data();
    layered::compute_residual(orig.data(), w, h,
                              sr.y.data(), sr.y_stride, sr.y_ps,
                              ru, sr.u_stride, sr.u_ps,
                              rv, sr.v_stride, sr.v_ps, sym.data());

    // 抽查残差值正确（Y 角点 + U/V 各一）
    for (int i = 0; i < 8; ++i) {
        std::size_t idx = (std::size_t)byte(rng) % sym.size();
        int expect_d = (int)orig[idx] - (int)recon[idx];
        if (expect_d < -128) expect_d = -128;
        if (expect_d > 127) expect_d = 127;
        CHECK(sym[idx] == (std::uint8_t)(expect_d + 128), "%s sym[%zu]", what, idx);
    }

    std::vector<std::uint8_t> coded;
    bool ok = layered::encode_enhancement(sym.data(), w, h, 512 * 1024, coded);
    CHECK(ok, "%s encode", what);
    if (!ok) return;

    std::vector<std::uint8_t> sym2(layered::frame_bytes(w, h), 0);
    ok = layered::decode_enhancement(coded.data(), coded.size(), w, h, sym2.data());
    CHECK(ok, "%s decode", what);
    CHECK(sym2 == sym, "%s roundtrip sym equal (coded=%zuB)", what, coded.size());

    std::vector<std::uint8_t> comp(layered::frame_bytes(w, h), 0);
    layered::apply_residual(recon.data(), sym2.data(), w, h, comp.data());
    CHECK(comp == orig, "%s composite == orig", what);
}

void test_all_zero() {
    const int w = 256, h = 128;
    std::vector<std::uint8_t> sym(layered::frame_bytes(w, h), 128);
    std::vector<std::uint8_t> coded;
    bool ok = layered::encode_enhancement(sym.data(), w, h, 512 * 1024, coded);
    CHECK(ok, "all-zero encode");
    // 全零：仅 3 字节头 + 位图（(256*128*3/2)/4096=12 段 → 2 字节）
    CHECK(coded.size() == 3 + 2, "all-zero size=%zu", coded.size());
    std::vector<std::uint8_t> sym2(sym.size(), 0);
    ok = layered::decode_enhancement(coded.data(), coded.size(), w, h, sym2.data());
    CHECK(ok && sym2 == sym, "all-zero roundtrip");
}

void test_worst_case_and_guard() {
    const int w = 128, h = 64;
    std::mt19937 rng(7);
    // 极端残差：全幅 ±128 均匀 → 逃逸路径全开
    std::vector<std::uint8_t> sym(layered::frame_bytes(w, h));
    std::uniform_int_distribution<int> byte(0, 255);
    for (auto& b : sym) b = (std::uint8_t)byte(rng);
    std::vector<std::uint8_t> coded;
    bool ok = layered::encode_enhancement(sym.data(), w, h, 16 * 1024 * 1024, coded);
    CHECK(ok, "worst-case encode");
    std::vector<std::uint8_t> sym2(sym.size(), 0);
    ok = layered::decode_enhancement(coded.data(), coded.size(), w, h, sym2.data());
    CHECK(ok && sym2 == sym, "worst-case roundtrip (coded=%zuB)", coded.size());
    // 尺寸护栏：小上限必须拒发
    ok = layered::encode_enhancement(sym.data(), w, h, 64, coded);
    CHECK(!ok, "size guard rejects");
}

void test_interleave_uv() {
    const int w = 8, h = 4;
    std::vector<std::uint8_t> sym(layered::frame_bytes(w, h));
    for (std::size_t i = 0; i < sym.size(); ++i) sym[i] = (std::uint8_t)i;
    std::vector<std::uint8_t> uv((std::size_t)(w / 2) * (h / 2) * 2, 0);
    layered::interleave_uv(sym.data(), w, h, uv.data());
    const std::size_t uoff = (std::size_t)w * h;
    for (int r = 0; r < h / 2; ++r)
        for (int c = 0; c < w / 2; ++c) {
            CHECK(uv[((std::size_t)r * (w / 2) + c) * 2] == sym[uoff + (std::size_t)r * (w / 2) + c],
                  "interleave U(%d,%d)", c, r);
            CHECK(uv[((std::size_t)r * (w / 2) + c) * 2 + 1] ==
                      sym[uoff + (std::size_t)(w / 2) * (h / 2) + (std::size_t)r * (w / 2) + c],
                  "interleave V(%d,%d)", c, r);
        }
}

void test_truncated_graceful() {
    const int w = 128, h = 64;
    std::mt19937 rng(11);
    std::vector<std::uint8_t> orig(layered::frame_bytes(w, h));
    std::uniform_int_distribution<int> byte(0, 255);
    for (auto& b : orig) b = (std::uint8_t)byte(rng);
    std::vector<std::uint8_t> recon = make_noisy_recon(orig, rng, 0.8);
    StridedRecon sr = make_strided(recon, w, h, false);
    std::vector<std::uint8_t> sym(layered::frame_bytes(w, h), 0);
    layered::compute_residual(orig.data(), w, h,
                              sr.y.data(), sr.y_stride, sr.y_ps,
                              sr.u.data(), sr.u_stride, sr.u_ps,
                              sr.v.data(), sr.v_stride, sr.v_ps, sym.data());
    std::vector<std::uint8_t> coded;
    CHECK(layered::encode_enhancement(sym.data(), w, h, 512 * 1024, coded), "trunc enc");
    // 截断到头部之后任意位置：不得崩溃，解码返回 true（超界位读 0 → 零残差填充）
    std::vector<std::uint8_t> sym2(sym.size(), 0);
    std::size_t cut = coded.size() > 20 ? coded.size() - 7 : coded.size();
    bool ok = layered::decode_enhancement(coded.data(), cut, w, h, sym2.data());
    CHECK(ok, "truncated decode graceful");
    // 头部截断（位图不全）→ 拒绝
    ok = layered::decode_enhancement(coded.data(), 2, w, h, sym2.data());
    CHECK(!ok, "header-truncated rejected");
}

void test_perf() {
    const int w = 886, h = 1920;  // 生产尺寸
    std::mt19937 rng(42);
    std::vector<std::uint8_t> orig(layered::frame_bytes(w, h));
    std::uniform_int_distribution<int> byte(0, 255);
    for (auto& b : orig) b = (std::uint8_t)byte(rng);
    std::vector<std::uint8_t> recon = make_noisy_recon(orig, rng, 0.85);
    StridedRecon sr = make_strided(recon, w, h, false);

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> sym(layered::frame_bytes(w, h), 0);
    layered::compute_residual(orig.data(), w, h,
                              sr.y.data(), sr.y_stride, sr.y_ps,
                              sr.u.data(), sr.u_stride, sr.u_ps,
                              sr.v.data(), sr.v_stride, sr.v_ps, sym.data());
    auto t1 = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> coded;
    bool ok = layered::encode_enhancement(sym.data(), w, h, 512 * 1024, coded);
    auto t2 = std::chrono::steady_clock::now();
    CHECK(ok, "perf encode (coded=%zuB)", coded.size());
    std::vector<std::uint8_t> sym2(sym.size(), 0);
    ok = layered::decode_enhancement(coded.data(), coded.size(), w, h, sym2.data());
    auto t3 = std::chrono::steady_clock::now();
    CHECK(ok && sym2 == sym, "perf roundtrip");
    std::printf("[perf] %dx%d residual=%.2fms encode=%.2fms decode=%.2fms coded=%zuKB (%.1f%%)\n",
                w, h,
                std::chrono::duration<double, std::milli>(t1 - t0).count(),
                std::chrono::duration<double, std::milli>(t2 - t1).count(),
                std::chrono::duration<double, std::milli>(t3 - t2).count(),
                coded.size() / 1024,
                100.0 * coded.size() / sym.size());
}

// 迷彩变换（方案A）：正/逆往返逐字节一致（变换本身无损），含同址调用
void test_camouflage(int w, int h, const char* what, std::mt19937& rng) {
    std::vector<std::uint8_t> sym(layered::frame_bytes(w, h));
    // 模拟残差分布：大块平坦（128 常数）+ 少量噪声/边界突变
    std::uniform_int_distribution<int> byte(0, 255);
    std::uniform_int_distribution<int> small(120, 136);
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c)
            sym[(std::size_t)r * w + c] = (r % 8 == 0 && c % 16 == 0)
                    ? (std::uint8_t)byte(rng) : (std::uint8_t)small(rng);
    for (std::size_t i = (std::size_t)w * h; i < sym.size(); ++i)
        sym[i] = (std::uint8_t)small(rng);

    std::vector<std::uint8_t> camo(sym.size());
    layered::camouflage_forward(sym.data(), w, h, camo.data());
    std::vector<std::uint8_t> back(sym.size());
    layered::camouflage_inverse(camo.data(), w, h, back.data());
    CHECK(back == sym, "%s roundtrip", what);

    // 同址调用
    std::vector<std::uint8_t> inplace = sym;
    layered::camouflage_forward(inplace.data(), w, h, inplace.data());
    CHECK(inplace == camo, "%s inplace forward", what);
    layered::camouflage_inverse(inplace.data(), w, h, inplace.data());
    CHECK(inplace == sym, "%s inplace inverse", what);

    // 平坦区差分后应为 0（黑平区，编码友好）：全 128 帧 → 仅各行首保留 128
    std::vector<std::uint8_t> flat(layered::frame_bytes(64, 32), 128);
    std::vector<std::uint8_t> fc(flat.size());
    layered::camouflage_forward(flat.data(), 64, 32, fc.data());
    const std::size_t fy = 64 * 32, fu = fy + 32 * 16;
    bool ok = true;
    for (std::size_t i = 0; i < fc.size(); ++i) {
        bool row_start = (i < fy && i % 64 == 0)
                || (i >= fy && i < fu && (i - fy) % 32 == 0)
                || (i >= fu && (i - fu) % 32 == 0);
        if (fc[i] != (row_start ? 128 : 0)) ok = false;
    }
    CHECK(ok, "%s flat->zero-diff", what);
}

}  // namespace

int main() {
    std::mt19937 rng(1234);
    test_roundtrip(128, 64, false, 0.85, rng, "planar small");
    test_roundtrip(128, 64, true, 0.85, rng, "semiplanar small");
    test_roundtrip(320, 240, false, 0.6, rng, "planar noisy");
    test_roundtrip(320, 240, true, 0.95, rng, "semiplanar mostlyzero");
    test_roundtrip(886, 1920, false, 0.85, rng, "planar production");
    test_all_zero();
    test_worst_case_and_guard();
    test_interleave_uv();
    test_truncated_graceful();
    test_camouflage(128, 64, "camo small", rng);
    test_camouflage(886, 1920, "camo production", rng);
    test_perf();

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d FAILURES\n", g_failures);
    return 1;
}
