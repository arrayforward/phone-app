#pragma once

// Internal per-peer state shared by the tight transport translation units
// (transport, reassembler, fragmenter, report). Not part of the public API.

#include "net/socket_platform.hpp"

#include "tight/types.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace tight::tight_detail {

struct PendingSend {
    PacketHeader m_header{};
    Bytes m_payload;
    std::chrono::steady_clock::time_point m_last_send;
    std::size_t m_bytes{0};
    std::uint32_t m_retries{0};
};

struct IncomingMessage {
    std::uint32_t m_message_id{};
    std::uint16_t m_total_count{};
    std::uint16_t m_data_count{};
    std::uint8_t m_channel{};   // 逻辑通道号（0=视频/数据，1=音频...）
    std::vector<std::optional<Bytes>> m_fragments;
    std::vector<std::uint16_t> m_sizes;
    std::chrono::steady_clock::time_point m_first_seen;
    // 首片发送 tick（帧级迟到统计基准：帧延迟 = 完成时刻 − 首片发送时刻）
    std::uint32_t m_first_tick_ms{};
};

// 接收端文件重组上下文（file 通道，按 file_id 索引）
struct FileRecv {
    std::string name;
    std::uint64_t total{};
    std::uint32_t chunk_size{};
    std::uint32_t chunk_count{};
    std::vector<std::optional<Bytes>> chunks;
    std::uint32_t received{};
};

struct Peer {
    std::mutex m_mu;
    std::string m_id;
    sockaddr_in m_addr{};
    bool m_addr_set{false};
    LinkRole m_role{LinkRole::Leaf};
    LinkState m_state{LinkState::Closed};
    std::uint32_t m_peer_client_id{};
    std::uint64_t m_peer_session_id{};
    std::chrono::steady_clock::time_point m_last_recv;
    std::chrono::steady_clock::time_point m_last_heartbeat_sent;
    std::chrono::steady_clock::time_point m_last_handshake_sent;
    // 握手重发退避（500ms 起步、封顶 5s），进入 Handshake 状态时重置。
    std::chrono::milliseconds m_handshake_backoff{500};
    std::chrono::steady_clock::time_point m_last_report_sent;
    // Online 通告重发时间戳：Established/Online 状态下按心跳周期重发
    // Online 报文（幂等），避免单次 Online 通告丢失导致对端应用层
    // 状态永久停在 Established（弱网丢包场景实测复现）。
    std::chrono::steady_clock::time_point m_last_online_sent;
    std::uint32_t m_sequence_out{1};
    // 控制包（Handshake/HandshakeAck/Online/Heartbeat/Bye）序列号独立
    // 计数器：与数据序列号 m_sequence_out 分离，否则控制包会污染数据
    // 面的缺口跟踪基准（控制包永远不以 Data 分片到达，导致基准卡死）。
    std::uint32_t m_control_seq_out{1};
    // 消息组分 id（fragment 组的 message_id）独立计数器。
    // 必须与数据序列号 m_sequence_out 分离：若共用，每条消息会消耗两个
    // 序号，使对端缺口跟踪出现永不到达的"幽灵序号"，全部误报为丢包，
    // 导致 ack 游标冻结、m_pending 无限堆积（内存泄漏）。
    std::uint32_t m_msg_id_out{1};
    // 数据面重传缓冲（仅可靠通道的 Data/Parity 分片）：由 Report 的 ack
    // 游标剪枝、NACK 重传消费。只存数据包——控制包的挂账在
    // m_control_pending，两者序号空间独立（m_sequence_out /
    // m_control_seq_out 都从 1 起步，同号常见），混存会让控制 Ack 误删
    // 数据挂账（丢失后永不重传）、数据 NACK 误重传控制包。
    std::map<std::uint32_t, PendingSend> m_pending;
    // ackable 控制包（Handshake/HandshakeAck/Online）的挂账：仅由
    // handle_ack（Ack 报文携带控制序号）消费；重传由状态机驱动（握手
    // 退避重发、Online 周期幂等重发），不经 m_pending。
    std::map<std::uint32_t, PendingSend> m_control_pending;
    std::map<std::uint32_t, IncomingMessage> m_incoming;
    std::map<std::uint32_t, FileRecv> m_files;   // file 通道重组上下文（file_id → 状态）
    std::map<std::uint32_t, std::chrono::steady_clock::time_point> m_completed;
    std::map<std::uint32_t, std::chrono::steady_clock::time_point> m_missing_seqs;
    // 缺口所属逻辑通道（seq → channel）：per-channel 可靠开关下，仅可靠
    // 通道的缺口上报 NACK，不可靠通道（视频纯 FEC）立即跳过。
    std::map<std::uint32_t, std::uint8_t> m_missing_channel;
    // 本端 per-channel 可靠配置拷贝（建 peer 时写入，报告缺口过滤用）。
    std::array<bool, 8> m_channel_reliable{};
    std::set<std::uint32_t> m_recv_seqs;
    std::uint32_t m_next_expected_seq{};
    bool m_seq_initialized{false};
    std::uint32_t m_sender_rtt_us{};
    bool m_reconnect{};

    // Clock sync (对表): offset = remote_clock - local_clock (µs), estimated
    // at handshake as (remote_tick - local_arrival) - rtt/2 and re-synced on
    // every heartbeat to track drift. Both ends store the peer's offset;
    // local time is never modified.
    std::int64_t m_clock_offset_us{0};
    bool m_clock_synced{false};
    bool m_clock_pending{false};
    std::uint32_t m_hs_tick{};
    std::uint64_t m_hs_arrival_ms{};
    // Receiver-side late-packet accounting (reset every report interval):
    // a data packet is late when its transit time exceeds the configured
    // multiple of the RTT (late_rtt_multiplier mode) or the dynamic line
    // P50 + late_buffer_ms (late_buffer_ms mode).
    std::uint64_t m_transit_samples{};
    std::uint64_t m_late_samples{};
    // 单程传输时间直方图（8ms/bin × 256 = 0~2048ms）：统计延迟曲线，
    // 周期末据此算 P50（迟到线基准）与超线比例 p，并做 P99.9 诊断。
    std::array<std::uint32_t, 256> m_latency_hist{};
    std::uint64_t m_hist_samples{};
    // 当前迟到线（μs，= P50 + late_buffer_ms×1000，每 report 周期刷新）。
    std::uint64_t m_late_line_us{};
    // 帧级迟到统计（每报告期清零）：帧延迟直方图（8ms/bin × 64 =
    // 0~512ms）+ 本窗最大帧大小 + 逐帧 (F,D) 对。发送端用 F/btl 折算
    // 合理到达时间判定迟到（关键帧突刺 = 帧自身传输，不误报）。
    std::array<std::uint32_t, 64> m_frame_latency_hist{};
    std::uint64_t m_frame_hist_samples{};
    std::uint32_t m_frame_max_bytes{0};
    std::vector<std::pair<std::uint16_t, std::uint16_t>> m_frame_pairs;  // (帧字节, 帧延迟 ms)
    // Sender-side FEC 档位（0=无冗余 1=1 片 2=熵公式），迟滞状态机使用。
    std::uint8_t m_fec_stage{0};
    // FEC 关闭标志（发送侧按平滑 RTT 判定，receiver 线程写、encode 线程
    // 读）：RTT 长期 >200ms（长距离或重拥塞）时 FEC 冗余让出带宽——少量
    // 阻塞时冗余恢复有用，大量阻塞时冗余本身挤占带宽加剧拥塞。
    std::atomic<bool> m_fec_disable{false};
    // 实际 FEC 冗余统计（encode 线程累计，transport 读取计算冗余率）：
    // 滑动窗口 1s 内发送的数据片/校验片总数，ratio = parity / data。
    // fragmenter 每消息累计；窗口过期（>1s）时计数清零重开。
    std::atomic<std::uint64_t> m_fec_data_pkts{0};
    std::atomic<std::uint64_t> m_fec_parity_pkts{0};
    std::atomic<std::uint64_t> m_fec_stat_ts{0};   // 窗口起点 unix ms
    // Receiver-side inbound wire bytes (reset every report interval):
    // 上报为投递率样本。真实接收速率由链路瓶颈决定，不受 ACK 游标
    // 跳缺（skip_gap）影响，是 BtlBw 的可靠测量源。
    std::uint64_t m_recv_bytes{};
    // Receiver-side ECN/L4S CE 标记与数据包计数（reset every report
    // interval）：ce_ratio = ce_marks / data_pkts 上报给发送端做比例响应。
    std::uint64_t m_ce_marks{};
    std::uint64_t m_data_pkts{};
    // Sender side: latest late-packet ratio reported by the peer; drives the
    // FEC redundancy rate and acts as the secondary bandwidth-gain signal.
    double m_peer_late_ratio{0.0};
    // 是否已收到过对端 report（决定起步 FEC 与分段档位是否生效）。
    bool m_have_late_report{false};
    // 最近一次收到对端报告的时刻：报告超时检测——持续收不到报告（默认
    // 3×report_interval）= 链路严重卡顿/断流，发送端乘性下降 btl 收敛
    // 到保守低码率（防打穿），报告恢复后由恢复台阶自然回升。
    std::chrono::steady_clock::time_point m_last_report_at;
    // 对端上报的单程延迟中位数 P50（ms）：发送端延迟信号码率控制。
    std::uint16_t m_peer_p50_ms{0};

    // Inbound speed-test train accounting (receiver side): wire bytes and
    // arrival span of the current train; finalized into m_probe_bw_bps and
    // attached to the next report so the sender can seed its estimator.
    std::chrono::steady_clock::time_point m_probe_first;
    std::chrono::steady_clock::time_point m_probe_last;
    std::uint64_t m_probe_bytes{};
    std::uint32_t m_probe_count{};
    std::uint64_t m_probe_bw_bps{};

    // Command channel: ordered control/button packets (single datagram, no
    // reassembly). Out-of-order packets are held for at most 3 RTT before
    // the gap is skipped; later arrivals of skipped sequences are dropped.
    std::uint32_t m_cmd_seq_out{1};
    std::uint32_t m_cmd_next_expected{};
    bool m_cmd_initialized{false};
    std::map<std::uint32_t, Bytes> m_cmd_held;
    std::chrono::steady_clock::time_point m_cmd_gap_since;

    // 加密状态：握手阶段 ECDH 协商出的会话密钥（AES-256-GCM），
    // 双方 client_id 排序拼接为 salt 经 HKDF-SHA256 派生。
    bool m_crypto_ready{false};
    std::array<std::uint8_t, 32> m_crypto_key{};

    // 异常消息丢弃日志：由配置 TightConfig::drop_log 决定（默认开），
    // lite_mode 端点自动关闭（静默丢弃）；建 peer 时写入。
    bool m_drop_log{true};
    // 重传协商：m_retransmit 为本端配置（决定是否生成 NACK）；
    // m_peer_retransmit 为对端握手通告（决定是否保留重传缓冲）。
    bool m_retransmit{true};
    bool m_peer_retransmit{true};
    // 限流时间戳（每 peer 每秒最多一条，防日志洪水）
    std::chrono::steady_clock::time_point m_oversize_log_ts{};
};

// A probe train is considered finished once no Probe packet has arrived for
// this gap (trains are sent back-to-back, so inter-packet gaps are tiny).
inline constexpr std::chrono::milliseconds kProbeTrainGap{20};

// Finalizes the in-flight probe train when the gap has elapsed: bandwidth
// = received wire bytes / (last arrival - first arrival). No-op otherwise.
// 注意：span 长的测量不直接丢弃——拥塞队列展宽出的低值会被带宽估计器
// 的投递率样本（on_delivery_rate）自然纠正；而慢链路（如 12.5KB/s）的
// 正确测量同样表现为长 span，丢弃反而让发送端保持乐观假设并发洪水。
inline void finalize_probe_train(Peer& peer, std::chrono::steady_clock::time_point now) {
    if (peer.m_probe_count == 0) return;
    if (now - peer.m_probe_last <= kProbeTrainGap) return;
    auto span_us = std::chrono::duration_cast<std::chrono::microseconds>(
                       peer.m_probe_last - peer.m_probe_first).count();
    if (peer.m_probe_count >= 2 && span_us > 0) {
        peer.m_probe_bw_bps = static_cast<std::uint64_t>(
            static_cast<double>(peer.m_probe_bytes) * 1000000.0 /
            static_cast<double>(span_us));
    }
    peer.m_probe_count = 0;
    peer.m_probe_bytes = 0;
}

// One-way transit time (µs) of a packet carrying the peer's send tick,
// converted into the local clock domain via the peer's clock offset.
// Returns -1 when the clock offset is not yet available or no tick is set.
inline std::int64_t transit_time_us(const Peer& peer, std::uint32_t tick,
                                    std::uint64_t arrival_ms) {
    if (!peer.m_clock_synced || tick == 0) return -1;
    std::uint32_t arrival_low = static_cast<std::uint32_t>(arrival_ms & 0xFFFFFFFFULL);
    std::int64_t t = static_cast<std::int64_t>(
                         static_cast<std::int32_t>(arrival_low - tick)) * 1000
                     + peer.m_clock_offset_us;
    return t >= 0 ? t : 0;
}

struct AddrKey {
    std::uint32_t m_addr;
    std::uint16_t m_port;
    bool operator<(const AddrKey& o) const {
        if (m_addr != o.m_addr) return m_addr < o.m_addr;
        return m_port < o.m_port;
    }
};

}
