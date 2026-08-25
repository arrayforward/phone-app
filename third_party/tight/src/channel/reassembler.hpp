#pragma once

// Internal inbound data path: receive-sequence gap tracking, fragment
// collection, FEC recovery and message delivery. Not part of the public API.

#include "tight/types.hpp"

#include <cstdint>
#include <functional>

namespace tight::tight_detail {

struct Peer;
struct IncomingMessage;

class Reassembler {
public:
    using DeliverCallback = std::function<void(Peer* peer, Bytes payload)>;
    // 重组失败通知：分片缺失数超过 FEC 校验片能力，消息无法还原而丢失。
    // 携带逻辑通道号（channel），应用层可区分哪条流（视频/音频）。
    using LossCallback = std::function<void(Peer* peer, std::uint8_t channel)>;

    // Handles a Data/Parity packet: updates receive-sequence bookkeeping,
    // late-packet accounting (a packet is late when its one-way transit time,
    // derived from its send tick and the per-peer clock offset, exceeds
    // late_multiplier * rtt_us, or the dynamic line P50+late_buffer_ms when
    // late_buffer_ms > 0), collects fragments, and delivers each completed
    // message (recovered via FEC when needed) through the callback.
    // FEC 无法还原的消息通过 on_message_loss 通知应用（丢帧止损/请求关键帧）。
    // max_message_bytes：单条消息上限，超出范围的分片组直接丢弃（防内存耗尽）。
    // 丢弃日志由 Peer::m_drop_log 控制（配置项，lite_mode 自动关闭）。
    // report_interval：接收端 Report 上报周期。可靠通道（per-channel ARQ）
    // 的消息等待窗口需覆盖 NACK 上报+重传往返（3 个报告周期），否则重传
    // 分片未到就被误判丢失；不可靠通道窗口为 max(250ms, 2×RTT)。
    static void handle_data(Peer& peer, const PacketHeader& header,
                            const Bytes& payload, std::uint32_t rtt_us,
                            double late_multiplier, std::uint32_t late_buffer_ms,
                            std::size_t max_message_bytes,
                            std::chrono::milliseconds report_interval,
                            const DeliverCallback& deliver,
                            const LossCallback& on_message_loss = {});

private:
    // loss_wait：消息宣告丢失前的等待窗口（首片到达起算）。分片可能
    // 仍在途中或等待重传，窗口内只收片不宣告；超时仍未补齐才判定丢失。
    static bool try_assemble(Peer& peer, IncomingMessage& in,
                             std::size_t max_message_bytes,
                             std::chrono::milliseconds loss_wait,
                             const DeliverCallback& deliver,
                             const LossCallback& on_message_loss);
};

}
