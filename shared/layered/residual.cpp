#include "residual.h"

#include "entropy.h"

namespace layered {

namespace {

inline std::uint8_t clamp_sym(int d) {
    if (d < -128) d = -128;
    if (d > 127) d = 127;
    return (std::uint8_t)(d + 128);
}

}  // namespace

void compute_residual(const std::uint8_t* orig, int w, int h,
                      const std::uint8_t* ry, int y_stride, int y_ps,
                      const std::uint8_t* ru, int u_stride, int u_ps,
                      const std::uint8_t* rv, int v_stride, int v_ps,
                      std::uint8_t* sym_out) {
    // Y 平面（全分辨率）
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* o = orig + (std::size_t)y * w;
        const std::uint8_t* r = ry + (std::size_t)y * y_stride;
        std::uint8_t* s = sym_out + (std::size_t)y * w;
        if (y_ps == 1) {
            for (int x = 0; x < w; ++x) s[x] = clamp_sym((int)o[x] - (int)r[x]);
        } else {
            for (int x = 0; x < w; ++x) s[x] = clamp_sym((int)o[x] - (int)r[(std::size_t)x * y_ps]);
        }
    }
    // U/V 平面（1/4 分辨率）
    const int cw = w / 2, ch = h / 2;
    const std::uint8_t* ou = orig + (std::size_t)w * h;
    const std::uint8_t* ov = ou + (std::size_t)cw * ch;
    std::uint8_t* su = sym_out + (std::size_t)w * h;
    std::uint8_t* sv = su + (std::size_t)cw * ch;
    for (int y = 0; y < ch; ++y) {
        const std::uint8_t* our = ou + (std::size_t)y * cw;
        const std::uint8_t* ovr = ov + (std::size_t)y * cw;
        const std::uint8_t* ruw = ru + (std::size_t)y * u_stride;
        const std::uint8_t* rvw = rv + (std::size_t)y * v_stride;
        std::uint8_t* suw = su + (std::size_t)y * cw;
        std::uint8_t* svw = sv + (std::size_t)y * cw;
        for (int x = 0; x < cw; ++x) {
            suw[x] = clamp_sym((int)our[x] - (int)ruw[(std::size_t)x * u_ps]);
            svw[x] = clamp_sym((int)ovr[x] - (int)rvw[(std::size_t)x * v_ps]);
        }
    }
}

void apply_residual(const std::uint8_t* base, const std::uint8_t* sym,
                    int w, int h, std::uint8_t* out) {
    const std::size_t n = frame_bytes(w, h);
    for (std::size_t i = 0; i < n; ++i) {
        int v = (int)base[i] + (int)sym[i] - 128;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        out[i] = (std::uint8_t)v;
    }
}

void interleave_uv(const std::uint8_t* sym, int w, int h, std::uint8_t* uv_out) {
    const int cw = w / 2, ch = h / 2;
    const std::uint8_t* u = sym + (std::size_t)w * h;
    const std::uint8_t* v = u + (std::size_t)cw * ch;
    for (int y = 0; y < ch; ++y) {
        const std::uint8_t* ur = u + (std::size_t)y * cw;
        const std::uint8_t* vr = v + (std::size_t)y * cw;
        std::uint8_t* o = uv_out + (std::size_t)y * cw * 2;
        for (int x = 0; x < cw; ++x) {
            o[x * 2] = ur[x];
            o[x * 2 + 1] = vr[x];
        }
    }
}

}  // namespace layered
