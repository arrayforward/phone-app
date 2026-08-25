#pragma once
// Intel oneVPL (libvpl) H.264 硬解器，接口与 H264Decoder 一致。
// 运行时动态加载 libvpl.dll（LoadLibrary，不依赖导入库），
// init 失败时由上层回退 Media Foundation 路径。

#include <cstddef>
#include <cstdint>
#include <memory>

#include "decoder.h"  // VideoFrame

class VplDecoder {
public:
    VplDecoder();
    ~VplDecoder();

    // 加载 libvpl.dll + 创建硬件解码会话。找不到硬件实现时返回 false（回退 MF）。
    bool init();
    void shutdown();

    // 送入一个 Annex-B 访问单元；本次调用解出帧时返回 true 并填充 out。
    // 解码器内部缓冲导致的延迟输出会在后续调用中吐出。
    bool decode(const std::uint8_t* data, std::size_t size, VideoFrame& out);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
