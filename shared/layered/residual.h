// 增强层残差平面计算/应用（协议 docs/protocol.md §3.4）。纯 C++，host 可单测。
//
// 残差符号平面 sym（frame_bytes(W,H) = W*H*3/2，Y+U+V planar 紧凑）：
//   sym[i] = clamp(orig[i] − recon[i], −128, 127) + 128    （128 = 零残差）
// orig 为紧凑 planar I420（repack 输出）；recon 为 MediaCodec 解码输出 Image
// 各平面（带 rowStride/pixelStride；pixelStride=2 为 NV12/NV21 半平面交错）。
#pragma once

#include <cstddef>
#include <cstdint>

namespace layered {

// sym = clamp(orig − recon, −128,127) + 128，输出紧凑 planar（Y W×H → U → V）。
// recon 三平面按各自 stride/pixelStride 读取。W、H 为偶数。
void compute_residual(const std::uint8_t* orig, int w, int h,
                      const std::uint8_t* ry, int y_stride, int y_ps,
                      const std::uint8_t* ru, int u_stride, int u_ps,
                      const std::uint8_t* rv, int v_stride, int v_ps,
                      std::uint8_t* sym_out);

// 合成：out = clamp(base + sym − 128, 0, 255)（base/out 均为紧凑 planar I420）。
// host 测试与调试用；客户端实际在 shader 里做同式相加（纹理域）。
void apply_residual(const std::uint8_t* base, const std::uint8_t* sym,
                    int w, int h, std::uint8_t* out);

// sym 的 U/V 平面交织为 NV12 序（客户端上传 LUMINANCE_ALPHA 纹理用）：
// uv_out[(by*(w/2)+bx)*2+0] = sym U(bx,by)，+1 = sym V(bx,by)；尺寸 (w/2)*(h/2)*2。
void interleave_uv(const std::uint8_t* sym, int w, int h, std::uint8_t* uv_out);

}  // namespace layered
