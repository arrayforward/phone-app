#include "channel/reassembler.hpp"

#include "core/peer.hpp"
#include "protocol/wire_format.hpp"

#include "tight/fec.hpp"
#include "tight/logger.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <vector>

#include <cstdio>
#ifdef TIGHT_DBG_PACKET_LOG
#define TIGHT_DBG_PRINTF(...) std::printf(__VA_ARGS__)
#else
#define TIGHT_DBG_PRINTF(...) ((void)0)
#endif

namespace tight::tight_detail {

namespace {

// 异常消息丢弃日志：每 peer 每秒最多一条（接收线程热路径，
// 恶意洪水时不能被日志拖垮）
void log_oversize_drop(Peer& peer, const char* reason, std::uint32_t msg_id,
                       std::size_t value, std::size_t limit) {
    auto now = std::chrono::steady_clock::now();
    if (now - peer.m_oversize_log_ts < std::chrono::seconds(1)) return;
    peer.m_oversize_log_ts = now;
    TIGHT_LOG_WARN(std::string("[tight] 丢弃异常消息(") + reason +
                   "): peer=" + peer.m_id +
                   " msg_id=" + std::to_string(msg_id) +
                   " value=" + std::to_string(value) +
                   " limit=" + std::to_string(limit));
}

} // namespace

void Reassembler::handle_data(Peer& peer, const PacketHeader& header,
                              const Bytes& payload, std::uint32_t rtt_us,
                              double late_multiplier, std::uint32_t late_buffer_ms,
                              std::size_t max_message_bytes,
                              std::chrono::milliseconds report_interval,
                              const DeliverCallback& deliver,
                              const LossCallback& on_message_loss) {
    std::uint32_t seq = header.sequence;
    auto now = std::chrono::steady_clock::now();
    {
        static std::atomic<std::uint64_t> dbg_rx_last{0};
        auto dbg_rx_now = std::chrono::steady_clock::now().time_since_epoch().count();
        if (dbg_rx_now - dbg_rx_last.load() > 1000000LL) {
            dbg_rx_last.store(dbg_rx_now);
            TIGHT_DBG_PRINTF("DBG rx-data: msg=%u idx=%u/%u ch=%u seq=%u\n",
                        (unsigned)header.message_id, (unsigned)header.fragment_index,
                        (unsigned)header.fragment_count,
                        (unsigned)channel_of(header.reserved), (unsigned)seq);
            fflush(stdout);
        }
    }

    // One-way transit via the per-peer clock offset (computed outside the
    // lock: the clock fields are written exclusively on this receiver
    // thread). -1 means the clock is not synced yet -> no accounting.
    std::int64_t transit_us = transit_time_us(peer, header.tick, unix_millis());
    {
        // m_missing_seqs / m_recv_seqs / m_next_expected_seq /
        // m_transit_samples / m_late_samples are also mutated by the report
        // builder on the reactor thread. Guard them with peer.m_mu or the
        // two threads corrupt the std::map (rb_tree) internals -> crash in
        // _S_rebalance_for_erase.
        std::lock_guard<std::mutex> seq_lock(peer.m_mu);

        // Late-packet accounting + latency histogram. late_buffer_ms > 0 时
        // 迟到线 = P50 + late_buffer_ms（上层业务指定，视频=16ms），迟到的
        // 报文在视频场景即"丢"（由 FEC 补）；否则回退 late_multiplier×RTT。
        // 关键帧报文（线上 reserved bit11）排除：大 IDR 突发的串行化排队
        // 是帧自身传输时间，不是链路拥塞——不污染 P50 与报文级迟到统计。
        if (transit_us >= 0 && !is_keyframe_packet(header.reserved)) {
            ++peer.m_transit_samples;
            std::uint64_t t = static_cast<std::uint64_t>(transit_us);
            std::size_t bin = t / 8000;  // 8ms/bin，覆盖 0~2048ms（含长尾与排队积压）
            if (bin >= peer.m_latency_hist.size()) bin = peer.m_latency_hist.size() - 1;
            ++peer.m_latency_hist[bin];
            ++peer.m_hist_samples;
            if (late_buffer_ms > 0) {
                if (peer.m_late_line_us > 0 && t > peer.m_late_line_us) {
                    ++peer.m_late_samples;
                }
            } else {
                std::uint64_t threshold_us = static_cast<std::uint64_t>(
                    late_multiplier * static_cast<double>(rtt_us > 0 ? rtt_us : 10000));
                if (t > threshold_us) {
                    ++peer.m_late_samples;
                }
            }
        }

        // seq == 0 为未编号报文（Parity 等）：不参与缺口跟踪，也不允许
        // 初始化序列基准。否则 Parity 先到会以 next_expected=1 初始化，
        // 而 seq 1/2 是握手等控制包、永远不会以 Data 到达，导致基准卡死：
        // 缺口被误报为丢包并引发控制包重传循环、ack_seq 冻结、m_pending
        // 与 m_recv_seqs 无限增长（内存泄漏）。
        if (seq == 0) {
            // 跳过节流统计之外的序列跟踪，但仍走下方的分片重组流程
        } else if (!peer.m_seq_initialized) {
            peer.m_next_expected_seq = seq + 1;
            peer.m_seq_initialized = true;
        } else if (seq >= peer.m_next_expected_seq) {
            for (std::uint32_t g = peer.m_next_expected_seq; g < seq; ++g) {
                if (!peer.m_recv_seqs.count(g)) {
                    peer.m_missing_seqs.emplace(g, now);
                    peer.m_missing_channel[g] = channel_of(header.reserved);
                }
            }
            peer.m_recv_seqs.insert(seq);
            while (peer.m_recv_seqs.count(peer.m_next_expected_seq)) {
                peer.m_recv_seqs.erase(peer.m_next_expected_seq);
                ++peer.m_next_expected_seq;
            }
        } else {
            auto mit = peer.m_missing_seqs.find(seq);
            if (mit != peer.m_missing_seqs.end()) {
                peer.m_missing_seqs.erase(mit);
                peer.m_missing_channel.erase(seq);
                TIGHT_DBG_PRINTF("PROBE gap-filled seq=%u\n", (unsigned)seq); fflush(stdout);
            }
            // 乱序/重传分片（seq < next_expected）到达：从缺失表移除后
            // 还要插入 recv_seqs 并推进游标——若它正是当前游标缺口，不
            // insert 会导致游标永久冻结（后续同序号包不会再来），ack 停
            // 滞、发送端 m_pending 无法按 ack 修剪，剩余缺口重传无对象。
            peer.m_recv_seqs.insert(seq);
            while (peer.m_recv_seqs.count(peer.m_next_expected_seq)) {
                peer.m_recv_seqs.erase(peer.m_next_expected_seq);
                ++peer.m_next_expected_seq;
            }
        }
    }

    if (header.fragment_count == 0) return;
    {
        std::lock_guard<std::mutex> lock(peer.m_mu);
        if (peer.m_completed.find(header.message_id) != peer.m_completed.end()) return;
    }
    std::uint16_t idx = header.fragment_index;
    std::uint16_t cnt = header.fragment_count;
    if (idx >= cnt) return;
    // 条目创建前按配置上限校验分片数：合法发送方的数据分片（除末片外）
    // 不小于 64 字节，超限的 fragment_count 必为异常/恶意，直接丢弃，
    // 防止 m_incoming 按虚假分片数预分配耗尽内存。
    const std::size_t max_fragments = max_message_bytes / 64 + 8;
    if (cnt > max_fragments) {
        if (peer.m_drop_log) {
            log_oversize_drop(peer, "fragment_count 超限", header.message_id, cnt, max_fragments);
        }
        return;
    }
    auto& in = peer.m_incoming[header.message_id];
    {
        std::lock_guard<std::mutex> lock(peer.m_mu);
        if (in.m_message_id == 0) {
            in.m_message_id = header.message_id;
            in.m_total_count = cnt;
            in.m_data_count = header.flags;
            in.m_channel = channel_of(header.reserved);   // 逻辑通道号（视频=0）
            in.m_fragments.assign(cnt, std::nullopt);
            in.m_sizes.assign(cnt, 0);
            in.m_first_seen = now;
            in.m_first_tick_ms = header.tick;
        }
        if (in.m_total_count != cnt) return;
        if (idx >= in.m_fragments.size()) return;
        if (!in.m_fragments[idx].has_value()) {
            in.m_fragments[idx] = payload;
            in.m_sizes[idx] = header.reserved & kRealSizeMask;
        }
    }
    // 等待窗口：分片全部到达或等待重传所需的时间。窗口内缺失分片
    // 数超过校验能力时只是"暂时无法组装"，不能宣告丢失（否则多分片
    // 消息的首片一到达就误报，见 try_assemble 注释）。
    // 可靠通道（per-channel ARQ）：NACK 在 Report 周期内上报、重传随后
    // 到达，窗口须对齐发送端重传放弃时限（report.cpp 的 kMaxRetries+2
    // 个报告周期，默认 1s×12=12s）——窗口短于放弃时限时，尾部丢包
    // （report/重传本身丢失）会耗尽窗口导致误判丢失，可靠通道语义
    // 应让重传有完整轮次；不可靠通道（纯 FEC 兜底，如实时视频）窗口
    // 取 max(250ms, 2×RTT) 快速止损。
    std::chrono::milliseconds loss_wait;
    std::uint8_t ch = channel_of(header.reserved);
    bool reliable = (ch < 8) && peer.m_channel_reliable[ch] && peer.m_peer_retransmit;
    if (reliable) {
        constexpr std::int64_t kReliableLossWaitPeriods = 12;
        loss_wait = std::chrono::milliseconds(kReliableLossWaitPeriods) *
                    report_interval.count();
    } else {
        loss_wait = std::max(std::chrono::milliseconds(250),
                             std::chrono::milliseconds(2 * (rtt_us / 1000)));
    }
    bool assembled = try_assemble(peer, in, max_message_bytes, loss_wait,
                                  deliver, on_message_loss);
    if (assembled) {
        std::lock_guard<std::mutex> lock(peer.m_mu);
        peer.m_completed[header.message_id] = now;
        peer.m_incoming.erase(header.message_id);
    }
}

bool Reassembler::try_assemble(Peer& peer, IncomingMessage& in,
                               std::size_t max_message_bytes,
                               std::chrono::milliseconds loss_wait,
                               const DeliverCallback& deliver,
                               const LossCallback& on_message_loss) {
    auto now = std::chrono::steady_clock::now();
    if (in.m_total_count < 1) return false;   // 单分片消息（total=1）正常投递
    std::size_t data_count = in.m_data_count > 0 ? in.m_data_count : (in.m_total_count - 1);
    std::size_t parity_count = in.m_total_count - data_count;
    std::size_t have = 0;
    for (std::size_t i = 0; i < data_count; ++i) {
        if (in.m_fragments[i].has_value()) ++have;
    }
    // 直接从分片组装完整消息：一次分配，跳过 4 字节总长前缀，
    // 不再为每个分片做中间拷贝。流的前 4 字节为总长（大端）。
    auto build_msg = [&](std::size_t data_count) -> Bytes {
        std::size_t stream_len = 0;
        for (std::size_t i = 0; i < data_count; ++i) {
            stream_len += std::min<std::size_t>(in.m_sizes[i], in.m_fragments[i]->size());
        }
        if (stream_len < 4) return Bytes{};
        // 读取流前 4 字节的总长前缀（可能横跨分片边界）
        std::uint8_t prefix[4] = {0, 0, 0, 0};
        std::size_t need = 4;
        for (std::size_t i = 0; i < data_count && need > 0; ++i) {
            std::size_t n = std::min<std::size_t>(in.m_sizes[i], in.m_fragments[i]->size());
            std::size_t take = std::min(n, need);
            std::memcpy(prefix + (4 - need), in.m_fragments[i]->data(), take);
            need -= take;
        }
        std::uint32_t total_be = 0;
        std::memcpy(&total_be, prefix, 4);
        std::uint32_t total = to_be32(total_be);
        // 报文声明总长超过配置上限：视为异常消息，丢弃不投递
        if (total > max_message_bytes) {
            if (peer.m_drop_log) {
                log_oversize_drop(peer, "消息总长超限", in.m_message_id, total, max_message_bytes);
            }
            return Bytes{};
        }
        if (total > stream_len - 4) total = static_cast<std::uint32_t>(stream_len - 4);

        Bytes result(total);
        std::size_t skip = 4;   // 跳过总长前缀
        std::size_t out_off = 0;
        for (std::size_t i = 0; i < data_count && out_off < total; ++i) {
            std::size_t n = std::min<std::size_t>(in.m_sizes[i], in.m_fragments[i]->size());
            const std::uint8_t* src = in.m_fragments[i]->data();
            if (skip >= n) { skip -= n; continue; }
            src += skip;
            n -= skip;
            skip = 0;
            if (out_off + n > total) n = total - out_off;
            std::memcpy(result.data() + out_off, src, n);
            out_off += n;
        }
        return result;
    };
    if (have == data_count) {
        for (std::size_t i = 0; i < data_count; ++i) {
            if (!in.m_fragments[i].has_value()) {
                TIGHT_LOG_DEBUG(std::string("[tight] try_assemble missing frag i=") + std::to_string(i) +
                                 " data_count=" + std::to_string(data_count) +
                                 " total=" + std::to_string(in.m_total_count));
                return false;  // race; retry next fragment
            }
        }
        Bytes msg = build_msg(data_count);
        // 帧级迟到统计（peer.m_mu 保护——报告线程周期清零/打包）：
        // 帧延迟 D = 完成时刻 − 首片发送 tick（经时钟偏移换算），帧大小
        // F = 消息线上字节。发送端用 F/btl 折算合理到达时间判定迟到——
        // 关键帧突刺（I 帧 40-60KB 在低带宽链路传输数十毫秒）是帧自身
        // 传输，不算迟到；持续超发排队超过最大帧合理时间才记迟到。
        if (!msg.empty()) {
            std::lock_guard<std::mutex> lk(peer.m_mu);
            std::int64_t transit_us =
                transit_time_us(peer, in.m_first_tick_ms, unix_millis());
            if (transit_us >= 0) {
                std::uint64_t t = static_cast<std::uint64_t>(transit_us);
                std::size_t fbin = t / 8000;  // 8ms/bin，覆盖 0~512ms
                if (fbin >= peer.m_frame_latency_hist.size()) {
                    fbin = peer.m_frame_latency_hist.size() - 1;
                }
                ++peer.m_frame_latency_hist[fbin];
                ++peer.m_frame_hist_samples;
                std::size_t fb = msg.size();
                if (fb > peer.m_frame_max_bytes) peer.m_frame_max_bytes = fb;
                if (peer.m_frame_pairs.size() < 48) {
                    peer.m_frame_pairs.emplace_back(
                        static_cast<std::uint16_t>(std::min<std::size_t>(fb, 65535)),
                        static_cast<std::uint16_t>(std::min<std::uint64_t>(t / 1000, 65535)));
                }
            }
        }
        deliver(&peer, std::move(msg));
        return true;
    }
    // 统计缺失的数据分片数
    std::size_t multi = 0;
    for (std::size_t i = 0; i < data_count; ++i) {
        if (!in.m_fragments[i].has_value()) ++multi;
    }
    if (multi > parity_count) {
        // 缺失超过 FEC 校验能力。注意：分片可能仍在途中或等待重传——
        // 多分片消息从首片到达到最后一片到达之间，multi 必然 > parity
        // （stage 0 时 parity=0），若立即宣告丢失会把每个分片到达都误报
        // 为一次丢帧（消息实际组装成功）。故必须等待 loss_wait 窗口，
        // 窗口内只收片不宣告；超时仍未补齐才确认消息丢失并终结条目。
        if (now - in.m_first_seen < loss_wait) {
            return false;   // 仍在等待窗口内：等待更多分片/重传
        }
        // 超时确认丢失：通知应用层（视频场景请求关键帧快速恢复），
        // 返回 true 让调用方终结条目（迟到分片将被 m_completed 丢弃）。
        static std::atomic<std::uint64_t> dbg_last{0};
        auto dbg_now = std::chrono::steady_clock::now().time_since_epoch().count();
        if (dbg_now - dbg_last.load() > 500000000LL) {  // 500ms 限频
            dbg_last.store(dbg_now);
            TIGHT_DBG_PRINTF("DBG fec-fail: data=%zu have=%zu multi=%zu parity=%zu total=%u ch=%u\n",
                        data_count, have, multi, parity_count, (unsigned)in.m_total_count,
                        (unsigned)in.m_channel);
            fflush(stdout);
        }
        if (on_message_loss) on_message_loss(&peer, in.m_channel);
        return true;   // 终结该消息（调用方写入 m_completed 并清掉条目）
    }
    if (multi == 0) return false;
    std::size_t width = 0;
    for (auto& f : in.m_fragments) {
        if (f.has_value()) width = std::max(width, f->size());
    }
    if (width == 0) return false;

    // Reed-Solomon 解码：任意 multi 个缺失分片用任意 multi 个校验分片恢复
    std::vector<std::optional<Bytes>> data_frags(data_count);
    for (std::size_t i = 0; i < data_count; ++i) data_frags[i] = in.m_fragments[i];
    std::vector<std::pair<std::size_t, Bytes>> parities;
    for (std::size_t p = 0; p < parity_count; ++p) {
        if (data_count + p < in.m_fragments.size() &&
            in.m_fragments[data_count + p].has_value()) {
            parities.emplace_back(p, *in.m_fragments[data_count + p]);
        }
    }
    if (multi > parities.size()) return false;
    if (!ReedSolomon::decode(data_frags, parities, width)) return false;

    // 回填恢复出的分片；真实长度由报文内 4 字节总长前缀在 build_msg 裁剪
    for (std::size_t i = 0; i < data_count; ++i) {
        if (!in.m_fragments[i].has_value() && data_frags[i].has_value()) {
            in.m_fragments[i] = *data_frags[i];
            in.m_sizes[i] = static_cast<std::uint16_t>(width);
        }
    }
    deliver(&peer, build_msg(data_count));
    return true;
}

}
