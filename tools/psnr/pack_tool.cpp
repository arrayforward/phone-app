// PSNR 对比工具·打包/解包（host 端，MinGW g++）：
//   pack    in.rgba W H out.yuv  —— RGBA_8888(W×H) → 双 YUV420 I420(2W×H)（协议 §3.1 raw）
//   unpack  in.yuv  W H out.rgb  —— 双 YUV420 I420(2W×H) → RGB888(W×H)（§3.1 逆变换）
//   pack2   in.rgba W H out.yuv  —— 同上但 YCoCg 打包（协议 §3.2）
//   unpack2 in.yuv  W H out.rgb  —— YCoCg 逆变换（repack_test.cpp::restore_rgb_ycocg 同款）
#include "../../server/jni/repack_core.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static std::vector<std::uint8_t> read_all(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open %s failed\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<std::uint8_t> buf((size_t)n);
    if (fread(buf.data(), 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read %s failed\n", path); exit(1); }
    fclose(f);
    return buf;
}

static void write_all(const char* path, const std::uint8_t* data, size_t n) {
    FILE* f = fopen(path, "wb");
    if (!f || fwrite(data, 1, n, f) != n) { fprintf(stderr, "write %s failed\n", path); exit(1); }
    fclose(f);
}

// 双 YUV420 → RGB888（协议 §3.1 逆变换；与 repack_test.cpp::restore_rgb 相同）
static void restore_rgb(const std::uint8_t* dual, int w, int h, std::uint8_t* rgb) {
    const int enc_w = 2 * w;
    const std::uint8_t* y_plane = dual;
    const std::uint8_t* u_plane = dual + (size_t)enc_w * h;
    const std::uint8_t* v_plane = u_plane + (size_t)w * h / 2;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int px = x & 1, py = y & 1;
            size_t cidx = (size_t)(y / 2) * w + (x / 2) + (py ? w / 2 : 0);
            std::uint8_t g = px ? v_plane[cidx] : u_plane[cidx];
            std::uint8_t* out = rgb + ((size_t)y * w + x) * 3;
            out[0] = y_plane[(size_t)y * enc_w + x];
            out[1] = g;
            out[2] = y_plane[(size_t)y * enc_w + w + x];
        }
    }
}

// 双 YUV420 YCoCg → RGB888（协议 §3.2 逆变换；与 repack_test.cpp::restore_rgb_ycocg 相同）
static void restore_rgb_ycocg(const std::uint8_t* dual, int w, int h, std::uint8_t* rgb) {
    const int enc_w = 2 * w;
    const std::uint8_t* y_plane = dual;
    const std::uint8_t* u_plane = dual + (size_t)enc_w * h;
    const std::uint8_t* v_plane = u_plane + (size_t)w * h / 2;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int px = x & 1, py = y & 1;
            size_t cidx = (size_t)(y / 2) * w + (x / 2) + (py ? w / 2 : 0);
            int cg = (px ? v_plane[cidx] : u_plane[cidx]) - 128;
            int co = y_plane[(size_t)y * enc_w + w + x] - 128;
            int yv = y_plane[(size_t)y * enc_w + x];
            std::uint8_t* out = rgb + ((size_t)y * w + x) * 3;
            out[0] = (std::uint8_t)std::min(255, std::max(0, yv - cg + co));
            out[1] = (std::uint8_t)std::min(255, std::max(0, yv + cg));
            out[2] = (std::uint8_t)std::min(255, std::max(0, yv - cg - co));
        }
    }
}

int main(int argc, char** argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: pack_tool pack|unpack|pack2|unpack2 in W H out\n");
        return 1;
    }
    const char* mode = argv[1];
    int w = atoi(argv[3]);
    int h = atoi(argv[4]);
    if (strcmp(mode, "pack") == 0 || strcmp(mode, "pack2") == 0) {
        auto rgba = read_all(argv[2]);
        std::vector<std::uint8_t> out(repack::dual_i420_size(w, h));
        if (mode[4] == '2') {
            repack::repack_rgba_to_dual_i420_ycocg(rgba.data(), w, h, w * 4, 4, out.data());
        } else {
            repack::repack_rgba_to_dual_i420(rgba.data(), w, h, w * 4, 4, out.data());
        }
        write_all(argv[5], out.data(), out.size());
        printf("packed(%s) %dx%d -> %zu bytes\n", mode, w, h, out.size());
    } else if (strcmp(mode, "unpack") == 0 || strcmp(mode, "unpack2") == 0) {
        auto yuv = read_all(argv[2]);
        std::vector<std::uint8_t> rgb((size_t)w * h * 3);
        if (mode[6] == '2') {
            restore_rgb_ycocg(yuv.data(), w, h, rgb.data());
        } else {
            restore_rgb(yuv.data(), w, h, rgb.data());
        }
        write_all(argv[5], rgb.data(), rgb.size());
        printf("unpacked(%s) %zu bytes -> %dx%d rgb\n", mode, yuv.size(), w, h);
    } else {
        fprintf(stderr, "unknown mode %s\n", mode);
        return 1;
    }
    return 0;
}
