// PSNR 对比测试·host 解码工具（一次性）：Annex-B .h264 文件 → NV12 raw dump。
// 直接复用生产解码器 client/src/vpl_decoder.cpp（oneVPL 硬解，验证过能出帧）。
//
// 用法：vpl_decode.exe in.h264 out.nv12
//   输出文件 = Y 平面（W×H 紧凑）+ 交错 UV 平面（W×H/2），
//   尺寸打印到 stdout（供 python 驱动解析）。
//
// 构建（MinGW）：
//   g++ -O2 -std=c++17 -I client/src -I third_party/libvpl/api \
//       tools/psnr/vpl_decode.cpp client/src/vpl_decoder.cpp -o tools/psnr/vpl_decode.exe
#include "../../client/src/vpl_decoder.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

static std::vector<std::uint8_t> read_all(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open %s failed\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<std::uint8_t> buf((size_t)n);
    if (fread(buf.data(), 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "read %s failed\n", path); exit(1);
    }
    fclose(f);
    return buf;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: vpl_decode in.h264 out.nv12\n");
        return 1;
    }
    std::vector<std::uint8_t> stream = read_all(argv[1]);

    VplDecoder dec;
    if (!dec.init()) {
        fprintf(stderr, "vpl init failed\n");
        return 1;
    }

    VideoFrame frame;
    bool got = dec.decode(stream.data(), stream.size(), frame);
    // 冲刷：内部缓冲的帧用空输入催吐
    for (int i = 0; !got && i < 64; ++i) {
        got = dec.decode(nullptr, 0, frame);
    }
    if (!got) {
        fprintf(stderr, "no frame decoded from %s\n", argv[1]);
        return 1;
    }

    FILE* f = fopen(argv[2], "wb");
    if (!f) { fprintf(stderr, "open %s failed\n", argv[2]); return 1; }
    fwrite(frame.y.data(), 1, frame.y.size(), f);
    fwrite(frame.uv.data(), 1, frame.uv.size(), f);
    fclose(f);
    printf("decoded %s -> %dx%d nv12 (%zu+%zu bytes)\n", argv[1],
           frame.width, frame.height, frame.y.size(), frame.uv.size());
    return 0;
}
