// host 端单测：repack_rgba_to_dual_i420 / fill_input_image 对照协议 §3.1 公式。
// 构建：g++ -O2 -std=c++17 -I server/jni server/jni/repack_core.cpp server/jni/repack_test.cpp -o repack_test
#include "repack_core.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// 已知渐变图案（确定性，覆盖 x/y 两个方向的奇偶）
inline std::uint8_t patR(int x, int y) { return static_cast<std::uint8_t>((x * 3 + y) & 0xFF); }
inline std::uint8_t patG(int x, int y) { return static_cast<std::uint8_t>((x + y * 5 + 1) & 0xFF); }
inline std::uint8_t patB(int x, int y) { return static_cast<std::uint8_t>(((x + y) ^ 0x5A) & 0xFF); }

std::vector<std::uint8_t> make_rgba(int w, int h, int row_stride, int pixel_stride) {
    std::vector<std::uint8_t> buf(static_cast<size_t>(row_stride) * (h - 1)
                                  + static_cast<size_t>(pixel_stride) * (w - 1) + 4, 0xEE);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::uint8_t* px = buf.data() + static_cast<size_t>(y) * row_stride
                               + static_cast<size_t>(x) * pixel_stride;
            px[0] = patR(x, y);
            px[1] = patG(x, y);
            px[2] = patB(x, y);
            px[3] = 0xFF;
        }
    }
    return buf;
}

// 按协议 §3.1 公式逐字节校验双 YUV420 布局
void check_dual_i420(const std::uint8_t* out, int w, int h, const char* what) {
    const int enc_w = 2 * w;
    const std::uint8_t* y_plane = out;
    const std::uint8_t* u_plane = out + static_cast<size_t>(enc_w) * h;
    const std::uint8_t* v_plane = u_plane + static_cast<size_t>(w) * h / 2;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            CHECK(y_plane[static_cast<size_t>(y) * enc_w + x] == patR(x, y),
                  "%s Y_A(%d,%d)", what, x, y);
            CHECK(y_plane[static_cast<size_t>(y) * enc_w + w + x] == patB(x, y),
                  "%s Y_B(%d,%d)", what, x, y);
        }
    }
    for (int by = 0; by < h / 2; ++by) {
        for (int bx = 0; bx < w / 2; ++bx) {
            size_t idx = static_cast<size_t>(by) * w + bx;
            CHECK(u_plane[idx] == patG(2 * bx, 2 * by), "%s U_A(%d,%d)", what, bx, by);
            CHECK(v_plane[idx] == patG(2 * bx + 1, 2 * by), "%s V_A(%d,%d)", what, bx, by);
            CHECK(u_plane[idx + w / 2] == patG(2 * bx, 2 * by + 1), "%s U_B(%d,%d)", what, bx, by);
            CHECK(v_plane[idx + w / 2] == patG(2 * bx + 1, 2 * by + 1), "%s V_B(%d,%d)",
                  what, bx, by);
        }
    }
}

// 参考"还原"：双 YUV420 → RGB888（client shader 公式的 host 版），验证往返无损
void restore_rgb(const std::uint8_t* dual, int w, int h, std::uint8_t* rgb) {
    const int enc_w = 2 * w;
    const std::uint8_t* y_plane = dual;
    const std::uint8_t* u_plane = dual + static_cast<size_t>(enc_w) * h;
    const std::uint8_t* v_plane = u_plane + static_cast<size_t>(w) * h / 2;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int px = x & 1, py = y & 1;
            size_t cidx = static_cast<size_t>(y / 2) * w + (x / 2) + (py ? w / 2 : 0);
            std::uint8_t g = px ? v_plane[cidx] : u_plane[cidx];
            std::uint8_t* out = rgb + (static_cast<size_t>(y) * w + x) * 3;
            out[0] = y_plane[static_cast<size_t>(y) * enc_w + x];
            out[1] = g;
            out[2] = y_plane[static_cast<size_t>(y) * enc_w + w + x];
        }
    }
}

void test_repack(int w, int h, int row_stride, int pixel_stride, const char* what) {
    std::vector<std::uint8_t> rgba = make_rgba(w, h, row_stride, pixel_stride);
    std::vector<std::uint8_t> out(repack::dual_i420_size(w, h), 0xCD);
    repack::repack_rgba_to_dual_i420(rgba.data(), w, h, row_stride, pixel_stride, out.data());
    check_dual_i420(out.data(), w, h, what);

    // 往返无损（结构性，H.264 有损除外）
    std::vector<std::uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    restore_rgb(out.data(), w, h, rgb.data());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* p = rgb.data() + (static_cast<size_t>(y) * w + x) * 3;
            CHECK(p[0] == patR(x, y) && p[1] == patG(x, y) && p[2] == patB(x, y),
                  "%s roundtrip (%d,%d)", what, x, y);
        }
    }
}

void test_fill_planar(int enc_w, int enc_h, int row_pad, const char* what) {
    const int cw = enc_w / 2, ch = enc_h / 2;
    std::vector<std::uint8_t> src(static_cast<size_t>(enc_w) * enc_h * 3 / 2);
    for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<std::uint8_t>(i * 7 + 3);

    const int y_rs = enc_w + row_pad;
    const int c_rs = cw + row_pad;
    std::vector<std::uint8_t> yb(static_cast<size_t>(y_rs) * enc_h, 0x11);
    std::vector<std::uint8_t> ub(static_cast<size_t>(c_rs) * ch, 0x22);
    std::vector<std::uint8_t> vb(static_cast<size_t>(c_rs) * ch, 0x33);
    repack::fill_input_image(src.data(), enc_w, enc_h,
                             yb.data(), y_rs, 1, ub.data(), c_rs, 1, vb.data(), c_rs, 1);
    for (int y = 0; y < enc_h; ++y) {
        CHECK(std::memcmp(yb.data() + static_cast<size_t>(y) * y_rs,
                          src.data() + static_cast<size_t>(y) * enc_w, enc_w) == 0,
              "%s Y row %d", what, y);
    }
    const std::uint8_t* su = src.data() + static_cast<size_t>(enc_w) * enc_h;
    const std::uint8_t* sv = su + static_cast<size_t>(cw) * ch;
    for (int y = 0; y < ch; ++y) {
        CHECK(std::memcmp(ub.data() + static_cast<size_t>(y) * c_rs,
                          su + static_cast<size_t>(y) * cw, cw) == 0, "%s U row %d", what, y);
        CHECK(std::memcmp(vb.data() + static_cast<size_t>(y) * c_rs,
                          sv + static_cast<size_t>(y) * cw, cw) == 0, "%s V row %d", what, y);
    }
}

// rgba_to_i420 对照 BT.601 limited range 浮点参考式（±1 容差，整数近似）
void test_rgba_to_i420(int w, int h, const char* what) {
    std::vector<std::uint8_t> rgba = make_rgba(w, h, w * 4, 4);
    std::vector<std::uint8_t> out(static_cast<size_t>(w) * h * 3 / 2, 0xCD);
    repack::rgba_to_i420(rgba.data(), w, h, w * 4, 4, out.data());

    const std::uint8_t* y_plane = out.data();
    const std::uint8_t* u_plane = out.data() + static_cast<size_t>(w) * h;
    const std::uint8_t* v_plane = u_plane + static_cast<size_t>(w / 2) * (h / 2);

    auto ref_y = [](int r, int g, int b) {
        return 16.0 + (65.481 * r + 128.553 * g + 24.966 * b) / 255.0;
    };
    auto ref_u = [](int r, int g, int b) {
        return 128.0 + (-37.797 * r - 74.203 * g + 112.0 * b) / 255.0;
    };
    auto ref_v = [](int r, int g, int b) {
        return 128.0 + (112.0 * r - 93.786 * g - 18.214 * b) / 255.0;
    };

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double ey = ref_y(patR(x, y), patG(x, y), patB(x, y));
            int ay = y_plane[static_cast<size_t>(y) * w + x];
            CHECK(ay >= (int)(ey - 1.0) && ay <= (int)(ey + 1.5),
                  "%s Y(%d,%d) got %d want ~%.1f", what, x, y, ay, ey);
        }
    }
    for (int by = 0; by < h / 2; ++by) {
        for (int bx = 0; bx < w / 2; ++bx) {
            double su = 0, sv = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    int px = 2 * bx + dx, py = 2 * by + dy;
                    su += ref_u(patR(px, py), patG(px, py), patB(px, py));
                    sv += ref_v(patR(px, py), patG(px, py), patB(px, py));
                }
            }
            double eu = su / 4.0, ev = sv / 4.0;
            int au = u_plane[static_cast<size_t>(by) * (w / 2) + bx];
            int av = v_plane[static_cast<size_t>(by) * (w / 2) + bx];
            CHECK(au >= (int)(eu - 1.5) && au <= (int)(eu + 1.5),
                  "%s U(%d,%d) got %d want ~%.1f", what, bx, by, au, eu);
            CHECK(av >= (int)(ev - 1.5) && av <= (int)(ev + 1.5),
                  "%s V(%d,%d) got %d want ~%.1f", what, bx, by, av, ev);
        }
    }
}

void test_fill_nv12(int enc_w, int enc_h, const char* what) {    const int cw = enc_w / 2, ch = enc_h / 2;
    std::vector<std::uint8_t> src(static_cast<size_t>(enc_w) * enc_h * 3 / 2);
    for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<std::uint8_t>(i * 5 + 1);

    std::vector<std::uint8_t> yb(static_cast<size_t>(enc_w) * enc_h, 0);
    // NV12：U/V 共享缓冲，U 在偶数偏移、V 在奇数偏移，pixelStride=2
    std::vector<std::uint8_t> uv(static_cast<size_t>(enc_w) * ch, 0);
    repack::fill_input_image(src.data(), enc_w, enc_h,
                             yb.data(), enc_w, 1,
                             uv.data(), enc_w, 2,
                             uv.data() + 1, enc_w, 2);
    CHECK(std::memcmp(yb.data(), src.data(), static_cast<size_t>(enc_w) * enc_h) == 0,
          "%s Y plane", what);
    const std::uint8_t* su = src.data() + static_cast<size_t>(enc_w) * enc_h;
    const std::uint8_t* sv = su + static_cast<size_t>(cw) * ch;
    for (int y = 0; y < ch; ++y) {
        for (int x = 0; x < cw; ++x) {
            CHECK(uv[static_cast<size_t>(y) * enc_w + 2 * x] == su[static_cast<size_t>(y) * cw + x],
                  "%s NV12 U(%d,%d)", what, x, y);
            CHECK(uv[static_cast<size_t>(y) * enc_w + 2 * x + 1] ==
                      sv[static_cast<size_t>(y) * cw + x],
                  "%s NV12 V(%d,%d)", what, x, y);
        }
    }
}

}  // namespace

// YCoCg 双 YUV420 的参考还原（协议 §3.2 逆变换的 host 版）：±2 舍入容差
void restore_rgb_ycocg(const std::uint8_t* dual, int w, int h, std::uint8_t* rgb) {
    const int enc_w = 2 * w;
    const std::uint8_t* y_plane = dual;
    const std::uint8_t* u_plane = dual + static_cast<size_t>(enc_w) * h;
    const std::uint8_t* v_plane = u_plane + static_cast<size_t>(w) * h / 2;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int px = x & 1, py = y & 1;
            size_t cidx = static_cast<size_t>(y / 2) * w + (x / 2) + (py ? w / 2 : 0);
            int cg = (px ? v_plane[cidx] : u_plane[cidx]) - 128;
            int co = y_plane[static_cast<size_t>(y) * enc_w + w + x] - 128;
            int yv = y_plane[static_cast<size_t>(y) * enc_w + x];
            std::uint8_t* out = rgb + (static_cast<size_t>(y) * w + x) * 3;
            out[0] = static_cast<std::uint8_t>(std::min(255, std::max(0, yv - cg + co)));
            out[1] = static_cast<std::uint8_t>(std::min(255, std::max(0, yv + cg)));
            out[2] = static_cast<std::uint8_t>(std::min(255, std::max(0, yv - cg - co)));
        }
    }
}

// YCoCg 打包往返：±2 容差（整数移位舍入）
void test_repack_ycocg(int w, int h, const char* what) {
    std::vector<std::uint8_t> rgba = make_rgba(w, h, w * 4, 4);
    std::vector<std::uint8_t> out(repack::dual_i420_size(w, h), 0xCD);
    repack::repack_rgba_to_dual_i420_ycocg(rgba.data(), w, h, w * 4, 4, out.data());

    std::vector<std::uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    restore_rgb_ycocg(out.data(), w, h, rgb.data());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* p = rgb.data() + (static_cast<size_t>(y) * w + x) * 3;
            int er = std::abs((int)p[0] - patR(x, y));
            int eg = std::abs((int)p[1] - patG(x, y));
            int eb = std::abs((int)p[2] - patB(x, y));
            CHECK(er <= 2 && eg <= 2 && eb <= 2,
                  "%s roundtrip (%d,%d) err %d/%d/%d", what, x, y, er, eg, eb);
        }
    }
}

int main() {
    // 紧凑布局 + 带行填充/pixelStride 的布局
    test_repack(64, 48, 64 * 4, 4, "repack tight");
    test_repack(886, 1920, 886 * 4, 4, "repack 886x1920");
    test_repack(64, 48, 64 * 4 + 32, 4, "repack rowStride pad");

    test_repack_ycocg(64, 48, "ycocg 64x48");
    test_repack_ycocg(886, 1920, "ycocg 886x1920");

    test_rgba_to_i420(64, 48, "i420 64x48");
    test_rgba_to_i420(886, 1920, "i420 886x1920");

    test_fill_planar(128, 96, 0, "fill planar tight");
    test_fill_planar(128, 96, 64, "fill planar padded");
    test_fill_nv12(128, 96, "fill nv12");

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d FAILURES\n", g_failures);
    return 1;
}
