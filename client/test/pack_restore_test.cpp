// 打包/还原公式自测（不经 GL）：按 protocol §3.1
//   打包：RGB888 → 双 YUV420 左右拼接（编码帧 2W×H，NV12 布局）
//   还原：两种实现——
//     1) restore_int    整数索引版，直接对应协议公式；
//     2) restore_shader 浮点坐标版，逐语句模拟 renderer.cpp 的 fragment
//        shader（归一化坐标 → floor/mod 奇偶 → texel center 采样 → NEAREST 取 texel）。
// 两者均须与原图逐像素一致（结构性无损）。

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

struct Packed {
    int W = 0, H = 0;                 // 逻辑尺寸；编码帧 2W×H
    std::vector<std::uint8_t> y;      // 2W×H
    std::vector<std::uint8_t> uv;     // (2W)×(H/2) 字节，交错 UV
};

// §3.1 打包：左半 Y_A=R、右半 Y_B=B；色度 2×2 块携带全分辨率 G
Packed pack(const std::vector<std::uint8_t>& rgb, int W, int H) {
    Packed p;
    p.W = W; p.H = H;
    const int EW = 2 * W;
    p.y.resize(static_cast<size_t>(EW) * H);
    p.uv.resize(static_cast<size_t>(EW) * (H / 2));
    auto R = [&](int x, int y) { return rgb[(static_cast<size_t>(y) * W + x) * 3 + 0]; };
    auto G = [&](int x, int y) { return rgb[(static_cast<size_t>(y) * W + x) * 3 + 1]; };
    auto B = [&](int x, int y) { return rgb[(static_cast<size_t>(y) * W + x) * 3 + 2]; };
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            p.y[static_cast<size_t>(y) * EW + x] = R(x, y);          // 左半
            p.y[static_cast<size_t>(y) * EW + W + x] = B(x, y);      // 右半
        }
    }
    for (int by = 0; by < H / 2; ++by) {
        for (int bx = 0; bx < W / 2; ++bx) {
            // A 半帧块在 UV 行字节 bx*2；B 半帧块右移 W/2 个块 = 字节 W + bx*2
            std::uint8_t* a = &p.uv[static_cast<size_t>(by) * EW + bx * 2];
            std::uint8_t* b = &p.uv[static_cast<size_t>(by) * EW + W + bx * 2];
            a[0] = G(2 * bx, 2 * by);        // U_A：左上
            a[1] = G(2 * bx + 1, 2 * by);    // V_A：右上
            b[0] = G(2 * bx, 2 * by + 1);    // U_B：左下
            b[1] = G(2 * bx + 1, 2 * by + 1);// V_B：右下
        }
    }
    return p;
}

// 还原实现 1：整数索引，直接对应协议公式
std::vector<std::uint8_t> restore_int(const Packed& p) {
    const int W = p.W, H = p.H, EW = 2 * W;
    std::vector<std::uint8_t> rgb(static_cast<size_t>(W) * H * 3);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            int px = x & 1, py = y & 1;
            int bx = x / 2, by = y / 2;
            std::uint8_t r = p.y[static_cast<size_t>(y) * EW + x];
            std::uint8_t b = p.y[static_cast<size_t>(y) * EW + W + x];
            const std::uint8_t* row = &p.uv[static_cast<size_t>(by) * EW];
            std::uint8_t g;
            if (py == 0) g = (px == 0) ? row[bx * 2] : row[bx * 2 + 1];          // U_A/V_A
            else         g = (px == 0) ? row[W + bx * 2] : row[W + bx * 2 + 1];  // U_B/V_B
            std::uint8_t* d = &rgb[(static_cast<size_t>(y) * W + x) * 3];
            d[0] = r; d[1] = g; d[2] = b;
        }
    }
    return rgb;
}

// 还原实现 2：逐语句模拟 fragment shader（浮点归一化坐标 + NEAREST 采样）
std::vector<std::uint8_t> restore_shader(const Packed& p) {
    const int W = p.W, H = p.H, EW = 2 * W;
    // 纹理 NEAREST 采样：归一化坐标 → texel 索引（clamp 到界内）
    auto sampleY = [&](float u, float v) {
        int ix = static_cast<int>(u * EW); if (ix < 0) ix = 0; if (ix > EW - 1) ix = EW - 1;
        int iy = static_cast<int>(v * H);  if (iy < 0) iy = 0; if (iy > H - 1)  iy = H - 1;
        return p.y[static_cast<size_t>(iy) * EW + ix];
    };
    // 返回 (luminance, alpha) = (U, V)
    auto sampleUV = [&](float u, float v, std::uint8_t& lu, std::uint8_t& al) {
        int ix = static_cast<int>(u * W);       if (ix < 0) ix = 0; if (ix > W - 1)     ix = W - 1;
        int iy = static_cast<int>(v * (H / 2)); if (iy < 0) iy = 0; if (iy > H / 2 - 1) iy = H / 2 - 1;
        lu = p.uv[static_cast<size_t>(iy) * EW + ix * 2];
        al = p.uv[static_cast<size_t>(iy) * EW + ix * 2 + 1];
    };

    std::vector<std::uint8_t> rgb(static_cast<size_t>(W) * H * 3);
    const float fW = static_cast<float>(W), fH = static_cast<float>(H);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            // v_tex：该逻辑像素中心对应的归一化坐标（quad 线性插值的结果）
            float tx = (x + 0.5f) / fW, ty = (y + 0.5f) / fH;
            // --- 以下与 kFragmentShaderSrc 逐语句对应 ---
            float pX = std::min(std::floor(tx * fW), fW - 1.0f);
            float pY = std::min(std::floor(ty * fH), fH - 1.0f);
            float px = std::fmod(pX, 2.0f);
            float py = std::fmod(pY, 2.0f);
            float ysu = 0.5f / fW, ysv = 1.0f / fH;                 // y_step
            std::uint8_t r = sampleY((pX + 0.5f) * ysu, (pY + 0.5f) * ysv);
            std::uint8_t b = sampleY((pX + fW + 0.5f) * ysu, (pY + 0.5f) * ysv);
            float bx = std::floor(pX * 0.5f), by = std::floor(pY * 0.5f);  // blk
            float uvsu = 1.0f / fW, uvsv = 2.0f / fH;               // uv_step
            std::uint8_t ca_r, ca_a, cb_r, cb_a;
            sampleUV((bx + 0.5f) * uvsu, (by + 0.5f) * uvsv, ca_r, ca_a);
            sampleUV((bx + fW * 0.5f + 0.5f) * uvsu, (by + 0.5f) * uvsv, cb_r, cb_a);
            std::uint8_t g;
            if (py < 0.5f) g = (px < 0.5f) ? ca_r : ca_a;
            else           g = (px < 0.5f) ? cb_r : cb_a;
            // --- shader 模拟结束 ---
            std::uint8_t* d = &rgb[(static_cast<size_t>(y) * W + x) * 3];
            d[0] = r; d[1] = g; d[2] = b;
        }
    }
    return rgb;
}

bool compare(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b,
             int W, int H, const char* tag) {
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            size_t px = i / 3;
            std::fprintf(stderr,
                         "FAIL[%s]: pixel (%zu,%zu) ch=%zu: expect %u got %u\n",
                         tag, px % W, px / W, i % 3, a[i], b[i]);
            return false;
        }
    }
    std::printf("OK  [%s] %dx%d lossless\n", tag, W, H);
    return true;
}

// 确定性伪随机图（xorshift），叠加结构性图案保证奇偶/边界都被打到
std::vector<std::uint8_t> make_image(int W, int H, std::uint32_t seed) {
    std::vector<std::uint8_t> rgb(static_cast<size_t>(W) * H * 3);
    std::uint32_t s = seed;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            std::uint8_t* d = &rgb[(static_cast<size_t>(y) * W + x) * 3];
            d[0] = static_cast<std::uint8_t>((x * 3 + y * 5 + (s & 0xFF)) & 0xFF);
            d[1] = static_cast<std::uint8_t>((x * 7 + y * 11 + (x & 1) * 13 +
                                              ((s >> 8) & 0xFF)) & 0xFF);
            d[2] = static_cast<std::uint8_t>(((x * 17) ^ (y * 31) ^
                                              ((s >> 16) & 0xFF)) & 0xFF);
        }
    }
    return rgb;
}

bool run_case(int W, int H, std::uint32_t seed) {
    auto rgb = make_image(W, H, seed);
    Packed p = pack(rgb, W, H);
    bool ok = compare(rgb, restore_int(p), W, H, "int   ");
    ok &= compare(rgb, restore_shader(p), W, H, "shader");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= run_case(2, 2, 0x12345678u);      // 最小帧：唯一 2×2 块
    ok &= run_case(4, 4, 0xDEADBEEFu);
    ok &= run_case(64, 48, 0x0F0F0F0Fu);
    ok &= run_case(254, 130, 0xCAFEBABEu);  // 非 2 的幂
    ok &= run_case(1080, 2400, 0xABCD1234u);// 竖屏量级
    std::printf(ok ? "ALL PASS\n" : "FAILED\n");
    return ok ? 0 : 1;
}
