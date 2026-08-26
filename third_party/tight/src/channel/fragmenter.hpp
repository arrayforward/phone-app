#pragma once

// Internal outbound data path: message fragmentation and rotating-parity FEC
// generation. Not part of the public API.

#include "tight/types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace tight::tight_detail {

struct Peer;

class Fragmenter {
public:
    // Called once per emitted fragment (all data fragments first, then all
    // parity fragments). frag_data/frag_len 为分片区间（调用方持有，
    // 回调内同步消费）；width 为线上补齐宽度（不足补零）。
    using SendFragmentCallback = std::function<void(Peer* peer, std::uint32_t msg_id,
        std::uint16_t idx, std::uint16_t cnt, std::uint16_t data_cnt,
        std::uint16_t real_size, const std::uint8_t* frag_data,
        std::size_t frag_len, std::size_t width, bool ackable, bool keyframe)>;

    // Splits payload into mtu-sized fragments, appends FEC parity fragments,
    // and emits every fragment through the callback. channel 为逻辑通道号
    // （写入数据报 flags），channel_fec_extra 为该通道叠加在 late_ratio
    // 自适应校验片之上的固定冗余；probe_extra_parity 为带宽探测额外追加
    // 的校验片数（真实冗余，用于无 L4S 时的 2× 探测）。
    // channel_fec_on=false 时该通道一律不生成校验片（起始保护/自适应/
    // 通道额外/探测全部归零）——"丢了就丢"的流量（如分层视频残差层）。
    static void fragment_and_send(Peer& peer, Bytes payload, std::size_t mtu,
                                  const SendFragmentCallback& send_fragment,
                                  std::uint8_t channel = 0,
                                  std::uint16_t channel_fec_extra = 0,
                                  std::uint16_t probe_extra_parity = 0,
                                  bool keyframe = false,
                                  bool channel_fec_on = true);

    // 分段 FEC：由超线比例 p（延迟 > P50+late_buffer_ms 的报文占比）驱动。
    //   stage 0：p < 0.3%（统计分辨率下限）→ 0 片（无长尾，不加 FEC）
    //   stage 1：0.3% ≤ p ≤ 1% → 1 片
    //   stage 2：p > 1% → 熵公式 ceil(data×max(H(p)×1.2, p))，无 2-6 封顶，
    //            仅 100 安全阀门（赌注正态曲线不会特别扁）
    // 档位切换带 ±20% 迟滞（fragment_and_send 内状态机，防振荡）。
    static std::uint16_t compute_parity_count_for(double late_ratio,
                                                  std::size_t data_count,
                                                  std::uint8_t stage);

private:
    static constexpr double kFecSafetyCoefficient = 1.2;
};

}
