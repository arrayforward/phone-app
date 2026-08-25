#pragma once

#include "tight/types.hpp"

#include <cstddef>
#include <cstdint>

namespace tight {

// 外部 socket 抽象接口（外部 socket 模式）。
//
// 默认模式下 tight 自己创建/绑定/维护一个 UDP socket；外部 socket 模式下
// 应用把已有的网络通道（UDP socket、WebSocket、TCP 连接、QUIC 流、进程间
// 通道等）适配为该接口交给 tight 使用，tight 不再创建任何 socket：
//
//   - 发送：tight 通过 send_to() 输出一个个完整的数据报（tight 报文自带
//     48B 头 + CRC，天然自描述）。底层是流式通道（TCP 等）时，由适配层
//     自行做报文分帧（如长度前缀）；WebSocket 可直接用二进制消息边界。
//   - 接收：应用在自己的收包循环里拿到一个完整数据报后，调用
//     TightTransport::inject_packet() 注入 tight（推模型，tight 不占用
//     任何接收线程，也不阻塞在外部 socket 上）。
//
// 实现要求：
//   - send_to() 会被 tight 内部任意工作线程（reactor/encode/sender 等）
//     并发调用，实现必须线程安全，且不得长时间阻塞（发送失败直接返回
//     false，该数据报按丢弃处理，可靠通道由 ARQ 重传兜底）。
//   - to 为逻辑目的地址（对端 connect() 时传入的地址；无真实地址语义
//     的通道——如已建立的单连接 WebSocket/TCP——可忽略该参数）。
//   - tight 不拥有该 socket：stop()/析构不会关闭它，生命周期由应用管理。
class ISocket {
public:
    virtual ~ISocket() = default;

    // 发送一个完整数据报。返回 false = 发送失败（报文丢弃）。
    virtual bool send_to(const NetAddress& to,
                         const std::uint8_t* data, std::size_t size) = 0;
};

} // namespace tight
