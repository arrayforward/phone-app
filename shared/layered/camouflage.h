// 残差"伪装"变换（方案A，docs/android_client_plan.md §3）：水平一阶差分把残差
// sym 平面变成类自然图像的平滑帧，诱导硬件编码器高效压缩。
//
// 可逆性：模 256 差分（d[i] = (s[i] − s[i−1]) & 0xFF，行首 d[0] = s[0]），
// 逆变换为逐行前缀和（mod 256）——变换本身逐字节无损，唯一损失环节是
// 硬件编解码。零残差平坦区（sym=128 常数）差分后为 0（黑平区），是 H.264
// 最好编的形态。
#pragma once

#include <cstdint>

namespace layered {

// 正向：sym 平面（planar：Y W×H + U/V 各 (W/2)×(H/2)，紧凑）就地/另存做
// 逐行模 256 一阶差分。in/out 可同址。W、H 为偶数。
void camouflage_forward(const std::uint8_t* in, int w, int h, std::uint8_t* out);

// 逆向：迷彩帧（硬件解码重建）逐行前缀和还原 sym 平面。in/out 可同址。
void camouflage_inverse(const std::uint8_t* in, int w, int h, std::uint8_t* out);

}  // namespace layered
