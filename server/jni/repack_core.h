// RGBA_8888 → 双 YUV420 左右拼接重排（协议 docs/protocol.md §3.1）核心实现。
// 纯 C++，不依赖 JNI/Android 头，便于 host 端单测（repack_test.cpp）。
#pragma once

#include <cstdint>
#include <cstddef>

namespace repack {

// 双 YUV420（2W×H I420）总字节数 = 2W*H*1.5 = 3WH（恰好 RGB888 字节数/像素）。
inline size_t dual_i420_size(int w, int h) {
    return static_cast<size_t>(w) * h * 3;
}

// RGBA_8888 帧（W×H，行序自上而下）重排为 2W×H I420：
//   dst 布局：Y 平面 2W×H，U 平面 W×H/2，V 平面 W×H/2。
//   Y_A(x,y)=R(x,y)（左半），Y_B(x,y)=B(x,y)（右半，Y 平面 x∈[W,2W)）；
//   U 平面行 by：左 W/2 = G(2bx,2by)，右 W/2 = G(2bx,2by+1)；
//   V 平面行 by：左 W/2 = G(2bx+1,2by)，右 W/2 = G(2bx+1,2by+1)。
// rgba 平面需处理 rowStride/pixelStride（ImageReader 实际布局）。W、H 必须为偶数。
void repack_rgba_to_dual_i420(const std::uint8_t* rgba, int w, int h,
                              int row_stride, int pixel_stride, std::uint8_t* dst);

// YCoCg 版双 YUV420（协议 §3.2）：布局与 repack_rgba_to_dual_i420 完全相同，
// 载荷语义替换为：
//   Y_A = Y'  = (R + 2G + B + 2) >> 2        （真亮度）
//   Y_B = Co  = ((R - B) >> 1) + 128         （橙差，偏置到 0..255）
//   色度 = Cg = ((2G - R - B) >> 2) + 128    （绿差，偏置到 0..255）
// 逆变换（client shader）：G=Y'+cg; B=Y'-cg-co; R=Y'-cg+co（±1~2 舍入）。
// 收益：UI 近灰画面色差通道平坦幅度小，色度几乎不耗码率 → 同码率画质更好。
void repack_rgba_to_dual_i420_ycocg(const std::uint8_t* rgba, int w, int h,
                                    int row_stride, int pixel_stride, std::uint8_t* dst);

// 单 YUV420 对照路径（PSNR 对比测试用）：RGBA_8888 → BT.601 limited range
// I420（W×H），色度按 2×2 盒式平均亚采样。dst 布局：Y 平面 W×H，
// U 平面 (W/2)×(H/2)，V 平面 (W/2)×(H/2)，总字节数 W*H*3/2。W、H 必须为偶数。
void rgba_to_i420(const std::uint8_t* rgba, int w, int h,
                  int row_stride, int pixel_stride, std::uint8_t* dst);

// 把 2W×H I420（repack_rgba_to_dual_i420 的输出）填入 MediaCodec 输入 Image 的
// 各平面，按各 plane 的 rowStride/pixelStride 逐平面拷贝。
// 兼容 planar 三平面（pixelStride=1）与 NV12/NV21 交错（pixelStride=2，
// 此时 U/V plane 缓冲交叠，按各自 stride 写入即天然交错）。
void fill_input_image(const std::uint8_t* src_i420, int enc_w, int enc_h,
                      std::uint8_t* y_buf, int y_row_stride, int y_pixel_stride,
                      std::uint8_t* u_buf, int u_row_stride, int u_pixel_stride,
                      std::uint8_t* v_buf, int v_row_stride, int v_pixel_stride);

}  // namespace repack
