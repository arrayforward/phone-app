#include "repack_core.h"

#include <cstring>

namespace repack {

void repack_rgba_to_dual_i420(const std::uint8_t* rgba, int w, int h,
                              int row_stride, int pixel_stride, std::uint8_t* dst) {
    const int enc_w = 2 * w;
    std::uint8_t* y_plane = dst;
    std::uint8_t* u_plane = dst + static_cast<size_t>(enc_w) * h;
    std::uint8_t* v_plane = u_plane + static_cast<size_t>(w) * h / 2;

    for (int y = 0; y < h; ++y) {
        const std::uint8_t* src = rgba + static_cast<size_t>(y) * row_stride;
        std::uint8_t* y_row = y_plane + static_cast<size_t>(y) * enc_w;
        // 色度平面行宽 = enc_w/2 = w；偶数行填左半（A 帧色度），奇数行填右半（B 帧色度）
        std::uint8_t* u_row = u_plane + static_cast<size_t>(y / 2) * w + (y & 1 ? w / 2 : 0);
        std::uint8_t* v_row = v_plane + static_cast<size_t>(y / 2) * w + (y & 1 ? w / 2 : 0);
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* px = src + static_cast<size_t>(x) * pixel_stride;
            y_row[x] = px[0];          // R → Y_A
            y_row[w + x] = px[2];      // B → Y_B
            if ((x & 1) == 0) {
                u_row[x >> 1] = px[1]; // G(2bx,  ·)
            } else {
                v_row[x >> 1] = px[1]; // G(2bx+1,·)
            }
        }
    }
}

static void copy_plane(const std::uint8_t* src, int src_stride,
                       std::uint8_t* dst, int dst_row_stride, int dst_pixel_stride,
                       int width, int height) {
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* s = src + static_cast<size_t>(y) * src_stride;
        std::uint8_t* d = dst + static_cast<size_t>(y) * dst_row_stride;
        if (dst_pixel_stride == 1) {
            std::memcpy(d, s, static_cast<size_t>(width));
        } else {
            for (int x = 0; x < width; ++x) {
                d[static_cast<size_t>(x) * dst_pixel_stride] = s[x];
            }
        }
    }
}

void fill_input_image(const std::uint8_t* src_i420, int enc_w, int enc_h,
                      std::uint8_t* y_buf, int y_row_stride, int y_pixel_stride,
                      std::uint8_t* u_buf, int u_row_stride, int u_pixel_stride,
                      std::uint8_t* v_buf, int v_row_stride, int v_pixel_stride) {
    const int cw = enc_w / 2;
    const int ch = enc_h / 2;
    const std::uint8_t* src_y = src_i420;
    const std::uint8_t* src_u = src_i420 + static_cast<size_t>(enc_w) * enc_h;
    const std::uint8_t* src_v = src_u + static_cast<size_t>(cw) * ch;

    copy_plane(src_y, enc_w, y_buf, y_row_stride, y_pixel_stride, enc_w, enc_h);
    copy_plane(src_u, cw, u_buf, u_row_stride, u_pixel_stride, cw, ch);
    copy_plane(src_v, cw, v_buf, v_row_stride, v_pixel_stride, cw, ch);
}

// BT.601 limited range（studio swing）整数近似：
//   Y  = 16  + ( 66*R + 129*G +  25*B + 128) / 256
//   Cb = 128 + (-38*R -  74*G + 112*B + 128) / 256
//   Cr = 128 + (112*R -  94*G -  18*B + 128) / 256
namespace {

inline std::uint8_t clamp255(int v) {
    return static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

inline int luma601(int r, int g, int b) {
    return ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
}

}  // namespace

void repack_rgba_to_dual_i420_ycocg(const std::uint8_t* rgba, int w, int h,
                                    int row_stride, int pixel_stride, std::uint8_t* dst) {
    const int enc_w = 2 * w;
    std::uint8_t* y_plane = dst;
    std::uint8_t* u_plane = dst + static_cast<size_t>(enc_w) * h;
    std::uint8_t* v_plane = u_plane + static_cast<size_t>(w) * h / 2;

    for (int y = 0; y < h; ++y) {
        const std::uint8_t* src = rgba + static_cast<size_t>(y) * row_stride;
        std::uint8_t* y_row = y_plane + static_cast<size_t>(y) * enc_w;
        std::uint8_t* u_row = u_plane + static_cast<size_t>(y / 2) * w + (y & 1 ? w / 2 : 0);
        std::uint8_t* v_row = v_plane + static_cast<size_t>(y / 2) * w + (y & 1 ? w / 2 : 0);
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* px = src + static_cast<size_t>(x) * pixel_stride;
            int r = px[0], g = px[1], b = px[2];
            // YCoCg（协议 §3.2）：真亮度 + 偏置色差
            y_row[x]     = static_cast<std::uint8_t>((r + 2 * g + b + 2) >> 2);   // Y'
            y_row[w + x] = static_cast<std::uint8_t>(((r - b) >> 1) + 128);       // Co
            int cg = ((2 * g - r - b) >> 2) + 128;                                // Cg
            if ((x & 1) == 0) {
                u_row[x >> 1] = static_cast<std::uint8_t>(cg);
            } else {
                v_row[x >> 1] = static_cast<std::uint8_t>(cg);
            }
        }
    }
}

void rgba_to_i420(const std::uint8_t* rgba, int w, int h,
                  int row_stride, int pixel_stride, std::uint8_t* dst) {
    std::uint8_t* y_plane = dst;
    std::uint8_t* u_plane = dst + static_cast<size_t>(w) * h;
    std::uint8_t* v_plane = u_plane + static_cast<size_t>(w / 2) * (h / 2);

    for (int y = 0; y < h; ++y) {
        const std::uint8_t* src = rgba + static_cast<size_t>(y) * row_stride;
        std::uint8_t* y_row = y_plane + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* px = src + static_cast<size_t>(x) * pixel_stride;
            y_row[x] = static_cast<std::uint8_t>(luma601(px[0], px[1], px[2]));
        }
    }
    // 2×2 盒式亚采样：块内 4 像素的 Cb/Cr 分别算术平均（+2 四舍五入）
    for (int by = 0; by < h / 2; ++by) {
        std::uint8_t* u_row = u_plane + static_cast<size_t>(by) * (w / 2);
        std::uint8_t* v_row = v_plane + static_cast<size_t>(by) * (w / 2);
        for (int bx = 0; bx < w / 2; ++bx) {
            int su = 0, sv = 0;
            for (int dy = 0; dy < 2; ++dy) {
                const std::uint8_t* src = rgba + static_cast<size_t>(2 * by + dy) * row_stride;
                for (int dx = 0; dx < 2; ++dx) {
                    const std::uint8_t* px = src + static_cast<size_t>(2 * bx + dx) * pixel_stride;
                    int r = px[0], g = px[1], b = px[2];
                    su += (-38 * r - 74 * g + 112 * b + 128) >> 8;
                    sv += (112 * r - 94 * g - 18 * b + 128) >> 8;
                }
            }
            u_row[bx] = clamp255(((su + 2) >> 2) + 128);
            v_row[bx] = clamp255(((sv + 2) >> 2) + 128);
        }
    }
}

}  // namespace repack
