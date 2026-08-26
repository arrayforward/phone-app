// 分层实验 host 端字节工具（MinGW g++，零第三方依赖）：
//   layer_tool crop  in.nv12 codedW codedH visW visH out.i420   NV12→I420 + 裁可视区
//   layer_tool uncamo in.i420 W H out.i420                      迷彩逆变换（逐行 mod256 前缀和）
//   layer_tool composite base.i420 enh.i420 W H out.i420        clamp(base + enh − 128)
//   layer_tool reconcmp a.i420 b.i420 W H                       逐字节比对（跨解码器确定性校验）
// 构建：g++ -O2 -std=c++17 tools/psnr/layer_tool.cpp -o tools/psnr/layer_tool.exe
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

std::vector<std::uint8_t> read_all(const char* path) {
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

void write_all(const char* path, const std::vector<std::uint8_t>& buf) {
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "open %s failed\n", path); exit(1); }
    fwrite(buf.data(), 1, buf.size(), f);
    fclose(f);
}

// NV12（codedW×codedH）→ I420（visW×visH，取左上可视区）
int crop(const char* in, int cw, int ch, int vw, int vh, const char* out_path) {
    std::vector<std::uint8_t> nv = read_all(in);
    if ((long)nv.size() < (long)cw * ch * 3 / 2) { fprintf(stderr, "nv12 size mismatch\n"); return 1; }
    std::vector<std::uint8_t> out((size_t)vw * vh * 3 / 2);
    // Y：逐行拷左半 vw 列
    for (int r = 0; r < vh; ++r)
        memcpy(out.data() + (size_t)r * vw, nv.data() + (size_t)r * cw, (size_t)vw);
    // UV：NV12 交错行（cw 字节 = cw/2 对）→ 拆 U/V 平面，取前 vw/2 对
    std::uint8_t* u = out.data() + (size_t)vw * vh;
    std::uint8_t* v = u + (size_t)(vw / 2) * (vh / 2);
    for (int r = 0; r < vh / 2; ++r) {
        const std::uint8_t* uvrow = nv.data() + (size_t)cw * ch + (size_t)r * cw;
        for (int c = 0; c < vw / 2; ++c) {
            u[(size_t)r * (vw / 2) + c] = uvrow[c * 2];
            v[(size_t)r * (vw / 2) + c] = uvrow[c * 2 + 1];
        }
    }
    write_all(out_path, out);
    printf("crop %s -> %s (%dx%d of %dx%d)\n", in, out_path, vw, vh, cw, ch);
    return 0;
}

// 迷彩逆变换：逐行 mod 256 前缀和（Y W×H + U/V 各 (W/2)×(H/2)）
int uncamo(const char* in_path, int w, int h, const char* out_path) {
    std::vector<std::uint8_t> d = read_all(in_path);
    if ((long)d.size() != (long)w * h * 3 / 2) { fprintf(stderr, "i420 size mismatch\n"); return 1; }
    std::vector<std::uint8_t> out(d.size());
    const size_t y_end = (size_t)w * h, u_end = y_end + (size_t)(w / 2) * (h / 2);
    auto sum_rows = [&](size_t begin, int width, int rows) {
        for (int r = 0; r < rows; ++r) {
            size_t base = begin + (size_t)r * width;
            int acc = 0;
            for (int x = 0; x < width; ++x) {
                acc = (acc + d[base + x]) & 0xFF;
                out[base + x] = (std::uint8_t)acc;
            }
        }
    };
    sum_rows(0, w, h);
    sum_rows(y_end, w / 2, h / 2);
    sum_rows(u_end, w / 2, h / 2);
    write_all(out_path, out);
    printf("uncamo %s -> %s\n", in_path, out_path);
    return 0;
}

// 合成：clamp(base + enh − 128)
int composite(const char* base_path, const char* enh_path, int w, int h, const char* out_path) {
    std::vector<std::uint8_t> b = read_all(base_path);
    std::vector<std::uint8_t> e = read_all(enh_path);
    size_t n = (size_t)w * h * 3 / 2;
    if (b.size() != n || e.size() != n) { fprintf(stderr, "composite size mismatch\n"); return 1; }
    std::vector<std::uint8_t> out(n);
    for (size_t i = 0; i < n; ++i) {
        int v = (int)b[i] + (int)e[i] - 128;
        out[i] = (std::uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    write_all(out_path, out);
    printf("composite %s + %s -> %s\n", base_path, enh_path, out_path);
    return 0;
}

// 逐字节比对：diff 字节数 / 最大差 / PSNR（同尺寸平面数据）
int reconcmp(const char* a_path, const char* b_path, int w, int h) {
    std::vector<std::uint8_t> a = read_all(a_path);
    std::vector<std::uint8_t> b = read_all(b_path);
    size_t n = (size_t)w * h * 3 / 2;
    if (a.size() != n || b.size() != n) { fprintf(stderr, "reconcmp size mismatch\n"); return 1; }
    size_t diffs = 0;
    int maxd = 0;
    double se = 0;
    for (size_t i = 0; i < n; ++i) {
        int d = (int)a[i] - (int)b[i];
        if (d) ++diffs;
        if (d < 0) d = -d;
        if (d > maxd) maxd = d;
        se += (double)d * d;
    }
    double mse = se / n;
    double psnr = mse == 0 ? 99.0 : 10.0 * log10(255.0 * 255.0 / mse);
    printf("reconcmp %s vs %s: diff_bytes=%zu/%zu maxd=%d psnr=%.2f\n",
           a_path, b_path, diffs, n, maxd, psnr);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: layer_tool crop|uncamo|composite|reconcmp ...\n");
        return 1;
    }
    if (!strcmp(argv[1], "crop") && argc == 8)
        return crop(argv[2], atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), atoi(argv[6]), argv[7]);
    if (!strcmp(argv[1], "uncamo") && argc == 6)
        return uncamo(argv[2], atoi(argv[3]), atoi(argv[4]), argv[5]);
    if (!strcmp(argv[1], "composite") && argc == 7)
        return composite(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]), argv[6]);
    if (!strcmp(argv[1], "reconcmp") && argc == 6)
        return reconcmp(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]));
    fprintf(stderr, "bad args\n");
    return 1;
}
