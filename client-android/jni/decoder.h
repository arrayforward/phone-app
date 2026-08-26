#pragma once
// Android H.264 解码器：NDK AMediaCodec 硬解，输出到 AImageReader
// （YUV_420_888），native 侧读平面转为 NV12 布局（Y + 交错 UV），
// 供 GLES2 shader 还原（与 Windows client 的 VideoFrame 同构）。
//
// 解码器按需惰性配置：首个 IDR 到达时解析其 SPS 得到显示尺寸（含裁剪），
// 提取 SPS/PPS 作为 csd-0/csd-1 配置；IDR 时尺寸变化（旋转/格式切换）自动重建。
// 全部方法须在解码线程调用（shutdown 除外）。

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// 诊断（华为 ROM 默认抑制应用 logcat）：解码器状态最近一次关键事件，
// 经 Client::stats 上抛 UI 状态条
extern std::string g_dec_status;

struct VideoFrame {
    int width = 0;    // 编码帧宽（double=2W，single=W）
    int height = 0;   // 编码帧高 H
    bool ycocg = false;   // 视频消息头 flags bit1（§3.2）
    bool single = false;  // 视频消息头 flags bit2（§3.3）
    bool has_residual = false;  // layer 模式：增强层赶上上屏（§3.4）
    std::uint64_t pts_ms = 0;   // 线上 pts（增强层配对键）
    std::vector<std::uint8_t> y;    // width*height，NV12 Y 平面（紧凑行）
    std::vector<std::uint8_t> uv;   // width*(height/2)，NV12 交错 UV
    std::vector<std::uint8_t> res_y;   // 残差 sym Y 平面（128=零残差）
    std::vector<std::uint8_t> res_uv;  // 残差 sym 交错 UV
};

class H264Decoder {
public:
    H264Decoder() = default;
    ~H264Decoder();

    // 送入一个 Annex-B 访问单元；解出帧时返回 true 并填充 out。
    // idr 为线上 flags bit0；非 IDR 且解码器未配置时返回 false。
    bool decode(const std::uint8_t* data, std::size_t size, bool idr, VideoFrame& out);
    void shutdown();

private:
    bool ensure_configured(const std::uint8_t* au, std::size_t size);
    void destroy();
    bool drain_one(VideoFrame& out);

    struct Impl;
    Impl* m = nullptr;  // 不透明 NDK 句柄集合（头文件不引 NDK 头）
};
