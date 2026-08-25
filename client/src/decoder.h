#pragma once
// Windows Media Foundation H.264 解码器（同步 MFT，零缓冲低时延）。
// 输入 Annex-B 访问单元（IDR 前须带 SPS/PPS，由 server 拼好），
// 输出 NV12 原样帧（编码帧 2W×H，protocol §3.1 双 YUV420 左右拼接）；
// RGB888 合并不再经过 CPU，由 renderer 的 GL shader 完成。

#include <cstdint>
#include <memory>
#include <vector>

struct VideoFrame {
    int width = 0;   // 编码帧宽 2W（逻辑帧宽 W = width/2）
    int height = 0;  // 编码帧高 H
    bool ycocg = false;  // 视频消息头 flags bit1：true=§3.2 YCoCg 打包
    std::vector<std::uint8_t> y;   // width*height，NV12 Y 平面（紧凑行，无 stride）
    std::vector<std::uint8_t> uv;  // width*(height/2)，NV12 交错 UV 平面
};

class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();

    // MFStartup + 创建 decoder MFT。须在解码所在线程调用（COM 按线程初始化）。
    bool init();
    void shutdown();

    // 送入一个 Annex-B 访问单元；解出帧时返回 true 并填充 out。
    // 解码器预热期（首帧前）返回 false 属正常。
    bool decode(const std::uint8_t* data, std::size_t size, VideoFrame& out);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
