#include "tight/bandwidth.hpp"

#include "tight/types.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>

namespace tight {

BandwidthEstimator::BandwidthEstimator(std::uint64_t initial_bytes_per_second)
    : m_btl_bw(std::max<std::uint64_t>(
          initial_bytes_per_second == 0 ? 1 : initial_bytes_per_second, kMinBtlBps)),
      m_btl_seed(m_btl_bw) {
}

void BandwidthEstimator::on_ack(std::size_t bytes, std::chrono::microseconds rtt) {
    (void)bytes;   // 投递率样本不再用于估计（AIMD 只依赖报告三信号）
    std::lock_guard<std::mutex> lock(m_mu);
    auto rtt_count = rtt.count();
    if (rtt_count <= 0) return;
    if (m_rt_prop.count() == 0 || rtt < m_rt_prop) m_rt_prop = rtt;
    if (m_rtt.count() == 0) {
        m_rtt = rtt;
    } else {
        m_rtt = std::chrono::microseconds((m_rtt.count() * 7 + rtt_count) / 8);
    }
}

void BandwidthEstimator::on_report(std::uint32_t p50_ms, double late_ratio,
                                   double loss_ratio, double ce_ratio,
                                   std::uint32_t rtt_us, bool pacer_limited,
                                   bool sustained_overload, bool in_evac_window,
                                   double recv_rate_bps,
                                   double sent_bytes_in_window) {
    (void)rtt_us;
    std::lock_guard<std::mutex> lock(m_mu);
    // 报告恢复到达：清除停滞标志（允许下一段停滞再次降速）
    m_report_stall = false;

    // RTprop：单程延迟最小值（对报告 P50 做 min filter）
    std::uint64_t p50_us = static_cast<std::uint64_t>(p50_ms) * 1000;
    if (m_rt_prop_us == 0 || p50_us < m_rt_prop_us) m_rt_prop_us = p50_us;

    // 排队延迟 = P50 - RTprop（EWMA 平滑趋势）
    double q_ms = (p50_us > m_rt_prop_us)
                      ? static_cast<double>(p50_us - m_rt_prop_us) / 1000.0
                      : 0.0;
    // 自引入排队归一：本窗口发送量按 btl 排空的中位排队时间（中位报文约
    // 排一半队）是自身传输时间——关键帧突发的主体。观测延迟未随发送量预期
    // 上升的部分不算拥塞；只有超出预期（真排队增长）才计入 delay 信号。
    if (sent_bytes_in_window > 0.0 && m_btl_bw > 0) {
        double expected_q_ms =
            sent_bytes_in_window / (2.0 * static_cast<double>(m_btl_bw)) * 1000.0;
        q_ms = q_ms > expected_q_ms ? q_ms - expected_q_ms : 0.0;
    }
    m_delay_ewma = 0.7 * m_delay_ewma + 0.3 * q_ms;

    // 拥塞判定（迟滞）：delay-based（排队延迟超阈值）、late-based（迟到
    // 率超阈值，含丢包）或 ECN（CE 标记占比超阈值）。恢复判定更严：
    // delay < 10ms 且 late < 0.5% 才提升。中间区保持——消除摆动。
    // 本地令牌桶限速（pacer_limited = 发送 < 供给）时，迟到率主体是"本地
    // 排队"伪信号：应用码率下限（QSV 1.5M）> 令牌 → outbound 积压 →
    // 接收端 p50 超线 → late 高——这不是链路拥塞，降速无益（发送已被
    // 令牌限制）且制造更多排队（L4S 实测：令牌卡死 → late 100% → 连降
    // 崩底 12.5K 死锁）。令牌卡时只用真实链路信号判定：丢包率（网络真
    // 丢）与 CE（proxy 链路积压）。非令牌受限时迟到率才是链路排队的
    // 真实反映，完整采用。
    // 拥塞判定（令牌卡死 pacer_limited = 本地令牌不足/token<0）：
    //   pacer_limited 时——发送已被令牌天然限速（≤btl×8，不会超发），late/
    //   loss 是"本地限速 → 播放端帧慢/缺"的伪信号（缺帧缺口不是链路丢
    //   包），只信 CE（proxy 直测队列积压，唯一不依赖发送端的链路信号）。
    //   无 CE 网络（真实网络常态）：pacer 期 cong=0 → 恢复爬升解卡（令牌
    //   转正后自然回到正常判定）。
    //   非 pacer 时——late/loss 是真实链路信号（迟到=排队、loss=丢包，
    //   late 覆盖丢包型拥塞），完整采用。
    bool congested = (m_delay_ewma > static_cast<double>(kDelayThresholdMs) && !pacer_limited) ||
                     (!pacer_limited &&
                      (late_ratio > kLateThreshold || loss_ratio > kLateThreshold)) ||
                     (ce_ratio > kCeThreshold);
    // 恢复判定：非令牌卡死时 late/loss 低于阈值即恢复；令牌卡死（本地
    // 暂停/令牌不足）时无 CE 即恢复（btl 提升解禁——缺帧伪信号不阻塞）
    bool recovered = (m_delay_ewma < static_cast<double>(kRecoverDelayMs)) &&
                     (pacer_limited ? (ce_ratio < kCeThreshold)
                                    : (late_ratio < kRecoverLateThreshold &&
                                       loss_ratio < kRecoverLateThreshold)) &&
                     !pacer_limited;
    m_congested = congested;

    if (congested) {
        if (in_evac_window) {
            // 排空窗口内：btl 冻结（不降不升）。量化大降（剧烈档）后进入
            // 排空窗口（video_capacity 按快照 Q 排空输出），窗口内 late/
            // CE/loss/delay 全部豁免——排空/追赶期的迟到信号是伪拥塞
            // （链路已按新 btl 排空，播放端在补历史欠账），继续降速是
            // 盲猜且会崩底；窗口结束（3s）后信号仍在才允许下一次下降
            // （进入新窗口，等一次排空期再决定下一次下降）。
        } else if (sustained_overload) {
            // 降速量化：**统一一张柔表**（kCongestTier1-4 = 0.90/0.75/0.65/
            // 0.50）——CE 与 late/loss 共享，信号强度（占比）决定档位，
            // 降幅一律柔和。理由（L4S 实测）：急表 → btl/码率大幅跳变 →
            // QSV 编码器频繁重启（4s 冷却下 60s 仍 17-36 次）→ 设备 -17
            // 崩溃 → 10-40s 断流。柔降让 btl 渐变收敛 → 码率小步变化 →
            // 重启次数降到与无 CE 网络相当 → 设备稳定。最大降 50% 走
            // slow 排空（3s Q 面积法，不跳帧）；降幅 <20%（×0.90）不触发
            // 排空窗口（网络负载平滑处理）。
            double strength = pacer_limited ? ce_ratio
                                            : std::max(std::max(late_ratio, loss_ratio),
                                                       ce_ratio);
            double factor;
            if (strength >= kCongestTier4Threshold) {
                factor = kCongestTier4Factor;
            } else if (strength >= kCongestTier3Threshold) {
                factor = kCongestTier3Factor;
            } else if (strength >= kCongestTier2Threshold) {
                factor = kCongestTier2Factor;
            } else if (strength >= kCeThreshold) {
                factor = kCongestTier1Factor;
            } else {
                // late/ce 都低于阈值但 delay 信号触发的拥塞（或信号统计窗口
                // 边界）：轻档兜底
                factor = kCongestTier1Factor;
            }
            m_btl_bw = std::max(static_cast<std::uint64_t>(
                                    static_cast<double>(m_btl_bw) * factor),
                                kMinBtlBps);
            // 降速时刻与因子记录（降幅 ≥20% 即 ×0.75/×0.65/×0.50）：transport
            // 以此为排空窗口起点，按降幅分流排空策略（统一柔表下全走 slow
            // 3s Q 面积法排空；降幅 <20% 的 ×0.90 不触发——网络负载平滑）
            if (factor <= kEvacTriggerMaxFactor) {
                m_last_congest_at = std::chrono::steady_clock::now();
                m_last_congest_factor = factor;
            }
            m_recover_step = 0;
            m_fec_probe_extra = 0;
        }
    } else if (!in_evac_window && (pacer_limited || recovered)) {
        // 排空窗口内恢复台阶同样冻结（btl 保持，排空期不折腾）；窗口
        // 结束（3s）后恢复 ×1.5 爬升自然开始。
        // 两步台阶法提升（防止提升负载带来卡顿），台阶间隔 = 一个报告周期：
        //   台阶 1（本报告）：btl ×1.5，压上 FEC 校验片负载感知链路——
        //            FEC 可丢失、不伤业务（对端缺校验片不影响数据片组装）；
        //            探测持续 1 个报告周期（333ms > 100ms 即可测量链路余量）
        //   台阶 2（下一报告，无拥塞才走到）：确认链路有余量 → btl ×1.5，
        //            移除 FEC 探测，业务流量自然替换 FEC 流量（video_capacity
        //            用实际冗余率折算，探测片移除后冗余率回落 → 编码码率上升）
        // 提升上限 = 初始种子（btl 不超过配置种子，防种子被台阶推高振荡）
        // 且不超过对端接收速率 ×1.2（快排跳帧清掉迟到信号后恢复台阶失去
        // 链路反馈，会立即爬满格 → 又超发 → 又降又排空的循环；recv_rate
        // 是链路真实吞吐，约束爬升收敛到链路上限附近。recv_rate≤0 不约束）。
        // 令牌卡死（pacer_limited）时 recv_rate ≈ 令牌速率（发送受本地限
        // 速）——用它约束爬升会自锁：btl 低 → 令牌低 → recv 低 → 爬升
        // 上限低 → 爬不动（实测恢复段 btl 冻结）。令牌受限期不约束（btl
        // 提升解禁，爬到令牌转正后由正常判定接管）；仅非令牌受限时用
        // recv_rate 约束（链路真实吞吐感知）。
        double recv_cap = std::numeric_limits<double>::max();
        if (recv_rate_bps > 0.0 && !pacer_limited) {
            recv_cap = std::max(static_cast<double>(m_btl_bw), recv_rate_bps * 1.2);
        }
        if (m_recover_step == 0) {
            m_btl_bw = static_cast<std::uint64_t>(
                std::min(static_cast<double>(m_btl_bw) * kRecoverFactor,
                         std::min(static_cast<double>(m_btl_seed), recv_cap)));
            if (m_btl_bw < kMinBtlBps) m_btl_bw = kMinBtlBps;
            // L4S 活跃（CE 标记存在）时无需 FEC 探测（CE 即链路反馈——
            // 探测冗余会使线上超发 → 更多 CE → 连降，L4S 实测自伤）；
            // 无 CE 环境用 FEC 探测感知链路余量
            m_fec_probe_extra = (ce_ratio < kCeThreshold) ? kProbeExtraParity : 0;
            m_recover_step = 1;
        } else if (m_recover_step == 1) {
            // 上一报告 FEC 探测无拥塞 → 业务替换 FEC（移除探测冗余）
            m_btl_bw = static_cast<std::uint64_t>(
                std::min(static_cast<double>(m_btl_bw) * kRecoverFactor,
                         std::min(static_cast<double>(m_btl_seed), recv_cap)));
            if (m_btl_bw < kMinBtlBps) m_btl_bw = kMinBtlBps;
            m_fec_probe_extra = 0;
            m_recover_step = 2;
        } else {
            // 一轮两步提升完成：回到 step0 开始下一轮（连续提升，直到
            // 种子上限或拥塞信号）。此前 step=2 保持导致 btl 卡在中途
            // （实测 703K 后不再爬升，永远到不了真实带宽）。
            m_recover_step = 0;
        }
    }
}

std::uint32_t BandwidthEstimator::fec_probe_extra() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_fec_probe_extra;
}

bool BandwidthEstimator::congested() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_congested;
}

std::chrono::steady_clock::time_point BandwidthEstimator::last_congest_at() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_last_congest_at;
}

double BandwidthEstimator::last_congest_factor() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_last_congest_factor;
}

void BandwidthEstimator::on_report_timeout() {
    std::lock_guard<std::mutex> lock(m_mu);
    // 每段停滞只降一次（m_report_stall 防重复叠加——停滞期间无反馈，
    // 反复减半是盲猜且下坠过冲，恢复 ×1.5/报告爬回很慢）。
    if (m_report_stall) return;
    m_report_stall = true;
    // 报告持续收不到 = 链路严重卡顿/断流：×0.5 单次降（下限防打穿），
    // 重置恢复台阶与 FEC 探测；报告恢复后恢复台阶（×1.5/报告）自然回升。
    m_btl_bw = std::max(static_cast<std::uint64_t>(
                            static_cast<double>(m_btl_bw) * kCongestFactor),
                        kMinBtlBps);
    m_recover_step = 0;
    m_fec_probe_extra = 0;
}

bool BandwidthEstimator::delay_congested() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_delay_ewma > static_cast<double>(kDelayThresholdMs);
}

std::uint64_t BandwidthEstimator::bytes_per_second() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return std::max<std::uint64_t>(m_floor, m_btl_bw);
}

std::chrono::microseconds BandwidthEstimator::rtt() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_rtt;
}

bool BandwidthEstimator::app_limited_state() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_app_limited;
}

std::uint64_t BandwidthEstimator::btl_bw_bps() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_btl_bw;
}

void BandwidthEstimator::set_seed_and_clamp(std::uint64_t probe_bps) {
    std::lock_guard<std::mutex> lock(m_mu);
    if (m_seed_calibrated || probe_bps == 0) return;   // 起步校准只一次
    m_seed_calibrated = true;
    // probe 结果 = 链路实测容量（bps → B/s）。只钳制 btl（起步不超实测
    // 链路），**不锁种子**——恢复爬升上限保持配置种子，链路变化自适应。
    // 注意：probe 在弱网握手期可能测偏（丢包/延迟 → 实测 682K vs 链路 5M），
    // 锁死 seed 会让 btl 永久卡在偏低值 → 视频超令牌 → 贷款循环永续；
    // 只钳制时偏低只是起步慢爬（×1.5/报告 ~3s 爬回链路真实值）。
    std::uint64_t cap_b = probe_bps / 8;
    if (cap_b == 0) cap_b = 1;
    if (m_btl_bw > cap_b) m_btl_bw = cap_b;
}

}  // namespace tight
