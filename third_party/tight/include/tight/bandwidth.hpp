#pragma once

// 三信号 AIMD 拥塞估计器（GCC 风格，替代 BBR 投递率/时间片循环）。
//
// 信号（全部来自对端报告，每 report_interval 评估一次）：
//   delay-based ：排队延迟 = P50 - RTprop（发送端对报告 P50 做 min filter），
//                 EWMA 平滑趋势
//   late-based  ：迟到率 p = 单程延迟超迟到线（P50+late_buffer）或 CE 标记
//                 的报文占比（替代丢包率——实时流语义：延迟超线即"丢"）
//   ECN/L4S    ：CE 标记报文在接收端计入迟到统计，自动并入 late-based
//
// 调整规则：
//   拥塞 → 按信号强度（迟到率/CE 占比的报文数量级）量化降幅，一次报告
//         收敛到位（强信号一次大降，弱信号温和降）：
//           strength ≥ 50% → ×0.20   （发送远超链路）
//           strength ≥ 20% → ×0.30
//           strength ≥ 5%  → ×0.45
//           strength ≥ 1%  → ×0.65   （轻）
//           仅 delay 信号   → ×0.65   （兜底）
//         其中 strength = max(late_ratio, ce_ratio)；pacer_limited（本地
//         令牌限速）时 late 是本地排队伪信号，用 max(loss_ratio, ce_ratio)
//         走同一阶梯。相比固定 ×0.5 每报告连降：超发积压排空期 CE 持续 →
//         连降 7 轮把 btl 崩到远低于真实链路（30M→0.23M）→ 视频下限都
//         "超发"→ 贷款循环；量化大降一次到位 → btl 收敛在真实链路附近，
//         CE 即停，不崩底不循环。
//   恢复（无拥塞）→ 两步台阶法：第一步 btl ×= 1.5，下一报告周期无拥塞再
//                 ×= 1.5（每步 +50%，两步共 +125%；台阶间隔一个报告周期
//                 观察，避免抖动误提）
//   btl 下限 100kbps（kMinBtlBps），防止长距离高 RTT 误判把 btl 打穿。

#include <chrono>
#include <cstdint>
#include <mutex>

namespace tight {

class BandwidthEstimator {
public:
    explicit BandwidthEstimator(std::uint64_t initial_bytes_per_second);

    // 每报告周期调用：三信号评估 + AIMD 调整。
    //  p50_ms    ：对端上报的单程延迟中位数
    //  late_ratio：帧级迟到率（帧延迟超"合理到达时间"（帧大小/btl+迟到
    //              buffer）的帧占比，0~1——关键帧突刺是帧自身传输不算
    //              迟到，持续超发排队才记迟到；丢包 = 永远迟到）
    //  ce_ratio  ：CE 标记占比（诊断，已并入 late_ratio 时不直接使用）
    //  rtt_us    ：对端 RTT（发送端平滑 RTT 由 on_ack 维护）
    //  pacer_limited：本地令牌桶限速中（发送被 btl 约束、本地排队）——此时
    //                p50 高是本地限速制造，非链路拥塞，不判拥塞（走恢复
    //                台阶），防止"btl 崩底 → 令牌<供给 → 自造积压 → 误判
    //                拥塞 → 更崩"的死锁
    //  sustained_overload：报告期平均发送速率 > 对端接收速率（持续超发）。
    //                关键帧突刺是瞬时信号（333ms 平均下 send ≤ recv）——
    //                overload=false 时 CE/late 是突刺瞬态，拥塞不降速
    //                （btl 保持，排空后恢复台阶自然回升），避免关键帧
    //                排队把 btl 打崩；持续超发才正常量化降速。
    //  in_evac_window：排空窗口内（量化大降后 video_capacity 排空输出期）。
    //                窗口内 btl 冻结（不降不升）：late/CE/loss/delay 全部
    //                豁免——排空/追赶期的迟到信号是伪拥塞（链路已按新 btl
    //                排空，播放端在补历史欠账），继续降速是盲猜；窗口结束
    //                （3s）后信号仍在才允许下一次下降（进入新窗口）。
    //  recv_rate_bps：对端实际接收速率（B/s）。恢复爬升上限约束：btl 不
    //                超过 recv_rate×1.2（快排跳帧清掉迟到信号后，恢复台阶
    //                失去链路反馈会立即爬满格 → 又超发 → 又降的循环；
    //                recv_rate 是链路真实吞吐，约束爬升收敛到链路上限）。
    //                recv_rate≤0（无流量上报）时不约束。
    //  sent_bytes_in_window：本报告窗口本端实际发送字节数（含音频/重传/
    //                控制）。用于 delay 信号的自引入排队归一：窗口发送量
    //                按 btl 排空的中位排队时间 = bytes/(2×btl) 是"自己制造
    //                的传输时间"（关键帧突发的主体），观测 P50−RTprop 超出
    //                该预期的部分才是真链路排队——关键帧不再推高 delay
    //                信号导致误判拥塞。≤0 = 未知（退回旧行为不归一）。
    void on_report(std::uint32_t p50_ms, double late_ratio, double loss_ratio,
                   double ce_ratio, std::uint32_t rtt_us, bool pacer_limited,
                   bool sustained_overload, bool in_evac_window,
                   double recv_rate_bps = -1.0, double sent_bytes_in_window = -1.0);

    // 当前 FEC 探测冗余片数（两步台阶提升的第一步使用）：恢复提升时先用
    // FEC 校验片压上负载感知链路（可丢失、不伤业务），第二步确认后移除。
    // fragmenter 据此刻意追加校验片（仅视频通道）。
    std::uint32_t fec_probe_extra() const;

    // 最近一次拥塞判定状态：拥塞（大量阻塞）时 FEC 冗余让出带宽
    // （fragmenter 校验片归零），避免"排队→迟到→FEC↑→更多排队"恶性循环。
    bool congested() const;

    // 最近一次剧烈降速时刻（量化阶梯 ×0.45 及以下档，strength≥5%）。
    // transport 据此判定排空窗口（窗口时长由 TightConfig::slowdown_window_ms
    // 决定）：窗口内 video_capacity 按下降幅度分流排空策略——
    //   ×0.45（中幅）→ 3 秒排空（Q 面积法：btl_snap − Q/窗口）
    //   ×0.30/×0.20（大幅）→ 快速排空（清队列 + 新 IDR）
    // time_point 无效值 = 未剧烈降速过。
    std::chrono::steady_clock::time_point last_congest_at() const;
    // 最近一次剧烈降速的量化因子（×0.45/×0.30/×0.20）：transport 据此
    // 选择排空策略（factor ≤ kEvacFastMaxFactor → 快速排空，否则 3 秒排空）。
    double last_congest_factor() const;

    // 对端报告超时（长时间收不到报告 = 链路严重卡顿/断流）：btl ×0.5
    // 单次降（×kCongestFactor，下限 kMinBtlBps 防打穿），重置恢复台阶与
    // FEC 探测。**每段停滞只降一次**（m_report_stall 标志防重复叠加——
    // 停滞期间无反馈，反复减半是盲猜且下坠过冲）；报告恢复到达时由
    // on_report 清除标志，之后按恢复台阶 ×1.5 自然回升。
    void on_report_timeout();

    // 排队型拥塞（排队延迟 EWMA > 20ms 阈值）：FEC 让出的专用判定——
    // 排队型拥塞冗余加剧排队，但"丢包型"拥塞（随机丢包）冗余有效对抗
    // 丢包，不能一并关闭（丢包场景回归实测 69 帧 nokey vs 全开 1 帧）。
    bool delay_congested() const;

    // RTT 样本（ACK/报告往返）：维护平滑 RTT（供 FEC 关闭、诊断等使用）。
    void on_ack(std::size_t bytes, std::chrono::microseconds rtt);

    std::uint64_t bytes_per_second() const;
    std::chrono::microseconds rtt() const;
    // 诊断：原始 BtlBw 测量值（bytes/s）
    std::uint64_t btl_bw_bps() const;
    // 起步带宽校准（probe 测速结果，bps）：握手后首个 probe 报告到达时
    // 调用一次——种子（恢复爬升上限）设为实测链路容量、btl 不超链路
    // （min 钳制），避免"固定 30M 种子在 4M 链路上硬塞 → 大步量化崩底"。
    void set_seed_and_clamp(std::uint64_t probe_bps);
    // 诊断保留：应用受限状态（AIMD 不依赖投递率，恒不更新）
    bool app_limited_state() const;

private:
    static constexpr std::uint64_t kMinBtlBps = 12500;      // 100kbps 下限（btl 估计值）
    static constexpr double kCongestFactor = 0.5;           // 遗留：固定降幅（已由量化阶梯替代）
    // 拥塞降速量化阶梯（按报文占比强度分级；strength = max(late, ce)，
    // pacer_limited 时 = max(loss, ce)）
    // 拥塞降速量化阶梯（统一柔表，按报文占比强度分级）：
    //   strength = max(late, loss, ce)——CE 与 late/loss 共享同一张表。
    //   柔降原因（L4S 实测）：CE 急表（×0.45/×0.30）→ btl/码率大幅跳变
    //   → QSV 编码器频繁重启（60s 17-36 次）→ 设备 -17 崩溃 → 10-40s
    //   断流。柔降让 btl 渐变收敛 → 码率小步变化 → 重启次数与无 CE 网络
    //   相当（~12-15 次）→ 设备稳定。最大降 50% 走 slow 排空（3s Q 面积
    //   法，不跳帧）；降幅 <20%（×0.90）不触发排空窗口（网络负载平滑）。
    static constexpr double kCongestTier1Factor = 0.90;     // 轻：1%~5%（降 10%）
    static constexpr double kCongestTier2Factor = 0.75;     // 中：5%~20%（降 25% → slow 排空）
    static constexpr double kCongestTier3Factor = 0.65;     // 重：20%~50%（降 35% → slow 排空）
    static constexpr double kCongestTier4Factor = 0.50;     // 极重：≥50%（降 50% → slow 排空）
    // 排空窗口触发上限：降幅 ≥20%（因子 ≤0.80）才进排空窗口（×0.90 轻档
    // 是网络负载平滑处理，不折腾）
    static constexpr double kEvacTriggerMaxFactor = 0.80;
    static constexpr double kCongestTier2Threshold = 0.05;  // 中档阈值（5%）
    static constexpr double kCongestTier3Threshold = 0.20;  // 重档阈值（20%）
    static constexpr double kCongestTier4Threshold = 0.50;  // 极重档阈值（50%）
    static constexpr double kRecoverFactor = 1.5;           // 恢复台阶：+50%
    static constexpr std::uint32_t kDelayThresholdMs = 20;    // 拥塞：排队延迟阈值
    static constexpr double kLateThreshold = 0.02;            // 拥塞：迟到率阈值 2%
    static constexpr double kCeThreshold = 0.01;              // 拥塞：CE 标记占比阈值 1%
    static constexpr std::uint32_t kRecoverDelayMs = 10;      // 恢复：排队延迟须 <10ms
    static constexpr double kRecoverLateThreshold = 0.005;    // 恢复：迟到率须 <0.5%
    static constexpr std::uint32_t kProbeExtraParity = 2;     // 提升第一步的 FEC 探测冗余片数

    mutable std::mutex m_mu;
    std::uint64_t m_btl_bw;
    std::uint64_t m_btl_seed;           // 初始种子（提升上限：btl 不超过种子）
    std::uint64_t m_floor{1024};
    std::uint64_t m_rt_prop_us{0};      // min(P50)（单程 RTprop）
    double m_delay_ewma{0.0};           // 排队延迟 EWMA（ms）
    int m_recover_step{0};              // 提升台阶状态：0=无 1=FEC探测 2=业务替换
    std::uint32_t m_fec_probe_extra{0}; // 当前 FEC 探测冗余（台阶 1 时 = kProbeExtraParity）
    bool m_congested{false};            // 最近一次判定的拥塞状态（FEC 让出带宽用）
    std::chrono::microseconds m_rtt{0};
    std::chrono::microseconds m_rt_prop{0};
    bool m_app_limited{false};
    // 最近一次剧烈降速（量化阶梯 ≤×0.65 档）时刻：排空窗口起点（transport
    // 比较 TightConfig::slowdown_window_ms 判定窗口）。无效值 = 从未触发。
    std::chrono::steady_clock::time_point m_last_congest_at{};
    // 上述降速的量化因子（×0.90/×0.75/×0.65/×0.50/×0.45/×0.30/×0.20）：
    // 排空策略分流（fast vs slow）。
    double m_last_congest_factor{1.0};
    // 起步校准标志：probe 测速结果已设置种子（只校准一次——起步语义）
    bool m_seed_calibrated{false};
    // 报告停滞标志：on_report_timeout 置位（每段停滞只降一次），
    // on_report 收到报告时清除（允许下一段停滞再次降速）。
    bool m_report_stall{false};
};

}  // namespace tight
