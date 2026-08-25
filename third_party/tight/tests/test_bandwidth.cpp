#include "test_framework.hpp"

#include "tight/bandwidth.hpp"

#include <chrono>

using namespace tight;
using namespace std::chrono_literals;

// on_report 参数：p50_ms, late_ratio, loss_ratio, ce_ratio, rtt_us,
//                 pacer_limited, sustained_overload, in_evac_window, recv_rate_bps
namespace {
void rep(BandwidthEstimator& e, std::uint32_t p50, double late, double loss = 0.0,
         double ce = 0.0, bool pacer = false, bool sustained = true,
         bool evac = false, double recv_bps = -1.0) {
    e.on_report(p50, late, loss, ce, 10000, pacer, sustained, evac, recv_bps);
}
} // namespace

TEST_CASE(bandwidth_initial_value) {
    BandwidthEstimator est(3750000); // 默认种子 30Mbps
    CHECK_EQ(est.bytes_per_second(), 3750000ULL);
    CHECK_EQ(est.btl_bw_bps(), 3750000ULL);
    CHECK_EQ(est.rtt().count(), 0);
}

TEST_CASE(bandwidth_zero_initial_gets_floor) {
    BandwidthEstimator est(0);
    CHECK(est.bytes_per_second() >= 1024);
}

TEST_CASE(bandwidth_min_btl_floor) {
    // 100kbps 下限（kMinBtlBps = 12500 B/s）
    BandwidthEstimator est(1);
    CHECK_GE(est.btl_bw_bps(), 12500ULL);
}

TEST_CASE(bandwidth_congestion_delay_only_light_tier) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // 报告 1：P50 建立 RTprop，无拥塞 → 恢复台阶 1（btl×1.5 被种子封顶）
    rep(est, 20, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024);
    CHECK_EQ(est.fec_probe_extra(), 2U);
    CHECK(!est.congested());
    // 报告 2：排队延迟 EWMA = 0.3×80 = 24ms > 20ms → 拥塞；
    // 持续超发 + 无 late/CE（strength=0）→ delay 是软信号 → 柔表轻档 ×0.90
    rep(est, 100, 0.0, 0.0, 0.0, false, true);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 90 / 100);
    CHECK_EQ(est.fec_probe_extra(), 0U);
    CHECK(est.congested());
    CHECK(est.delay_congested());
    // 轻档（×0.90）不触发排空窗口
    CHECK_EQ(est.last_congest_at().time_since_epoch().count(), 0);
}

TEST_CASE(bandwidth_unified_soft_table_tiers) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // 统一柔表（CE 与 late/loss 共享）：strength = max(late, loss, ce)
    // strength ≥ 50% → ×0.50
    rep(est, 10, 0.6);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 50 / 100);
    // 20%~50% → ×0.65
    rep(est, 10, 0.3);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 50 / 100 * 65 / 100);
    // 5%~20% → ×0.75（记录排空窗口因子）
    rep(est, 10, 0.08);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 50 / 100 * 65 / 100 * 75 / 100);
    CHECK_EQ(est.last_congest_factor(), 0.75);
    // 1%~5% → ×0.90（>2% 拥塞阈值才触发）
    rep(est, 10, 0.03);
    CHECK_EQ(est.btl_bw_bps(),
             10ULL * 1024 * 1024 * 50 / 100 * 65 / 100 * 75 / 100 * 90 / 100);
    // ×0.90 档（降幅 <20%）不更新排空窗口因子（保留 ×0.75 步的值）
    CHECK_EQ(est.last_congest_factor(), 0.75);
}

TEST_CASE(bandwidth_ce_shares_soft_table) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // CE 与 late/loss 共享同一张柔表（急表已废弃：L4S 实测急表 → btl/码率
    // 大幅跳变 → QSV 编码器频繁重启 → 设备崩溃/断流）
    // strength ≥ 50% → ×0.50
    rep(est, 10, 0.0, 0.0, 0.6);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 50 / 100);
    // 20%~50% → ×0.65
    rep(est, 10, 0.0, 0.0, 0.3);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 50 / 100 * 65 / 100);
    // 5%~20% → ×0.75（记录排空窗口因子）
    rep(est, 10, 0.0, 0.0, 0.08);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 50 / 100 * 65 / 100 * 75 / 100);
    CHECK_EQ(est.last_congest_factor(), 0.75);
    // 1%~5% → ×0.90
    rep(est, 10, 0.0, 0.0, 0.03);
    CHECK_EQ(est.btl_bw_bps(),
             10ULL * 1024 * 1024 * 50 / 100 * 65 / 100 * 75 / 100 * 90 / 100);
    CHECK_EQ(est.last_congest_factor(), 0.75);
}

TEST_CASE(bandwidth_sustained_overload_gate) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // 无持续超发（关键帧突刺）：CE/late 是瞬时信号，不降速
    rep(est, 10, 0.5, 0.0, 0.0, false, false);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024);
    CHECK(est.congested()); // 信号仍在，但 btl 保持
    // 持续超发 → late 主导柔表 ×0.50
    rep(est, 10, 0.5, 0.0, 0.0, false, true);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 50 / 100);
}

TEST_CASE(bandwidth_evac_window_freezes_btl) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // 先发生剧烈降速进入排空窗口
    rep(est, 10, 0.0, 0.0, 0.6); // CE ×0.50 → 5MB
    CHECK_EQ(est.btl_bw_bps(), 5ULL * 1024 * 1024);
    // 排空窗口内：信号全部豁免——再强的 late/CE 也不降，恢复也不升
    rep(est, 100, 0.9, 0.0, 0.9, false, true, true);
    CHECK_EQ(est.btl_bw_bps(), 5ULL * 1024 * 1024);
    rep(est, 10, 0.0, 0.0, 0.0, false, true, true);
    CHECK_EQ(est.btl_bw_bps(), 5ULL * 1024 * 1024); // 恢复台阶同样冻结
    // 窗口结束后：信号仍在 → 允许下一次下降（进入新窗口）
    rep(est, 100, 0.9, 0.0, 0.9, false, true, false);
    CHECK_EQ(est.btl_bw_bps(), 5ULL * 1024 * 1024 * 50 / 100);
}

TEST_CASE(bandwidth_ce_ratio_triggers_congestion) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // CE 占比 2%（strength=0.02）→ 统一柔表轻档 ×0.90
    rep(est, 10, 0.0, 0.0, 0.02);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 90 / 100);
    CHECK(est.congested());
}

TEST_CASE(bandwidth_pacer_limited_trusts_ce_only) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // 令牌受限（pacer=true）：late/loss 是本地限速/播放端帧慢的伪信号，
    // 只信 CE——无 CE 时高 late 高 loss 也不判拥塞（恢复爬升解卡）
    rep(est, 10, 0.9, 0.05, 0.0, true, true);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024); // 未被拖崩
    CHECK(!est.congested());
    // CE 高 = 真实超发（proxy 直测积压）→ 照降（strength = ce，统一柔表）
    rep(est, 10, 0.9, 0.05, 0.08, true, true);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 75 / 100);
    CHECK(est.congested());
    CHECK_EQ(est.last_congest_factor(), 0.75);
}

TEST_CASE(bandwidth_recovery_two_step_ramp) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // 先剧烈拥塞降到底：late 主导 ×0.50 → 5MB
    rep(est, 10, 0.5);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 / 2);
    // 台阶 1：btl ×1.5 = 7.5MB + FEC 探测冗余
    rep(est, 10, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 / 2 * 3 / 2);
    CHECK_EQ(est.fec_probe_extra(), 2U);
    // 台阶 2：×1.5 = 11.25MB（种子 10MB 封顶），移除 FEC 探测
    rep(est, 10, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024);
    CHECK_EQ(est.fec_probe_extra(), 0U);
    // 台阶走完回到起点（连续爬升）
    rep(est, 10, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024);
}

TEST_CASE(bandwidth_recovery_capped_by_seed) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // 拥塞减半后恢复提升，最多回到种子 10MB
    rep(est, 10, 0.5);
    CHECK_EQ(est.btl_bw_bps(), 5ULL * 1024 * 1024);
    rep(est, 10, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 5ULL * 1024 * 1024 * 3 / 2);  // 7.5MB
    rep(est, 10, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024);          // 封顶种子
    rep(est, 10, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024);          // 不再提升
    CHECK_EQ(est.fec_probe_extra(), 0U);
}

TEST_CASE(bandwidth_recovery_capped_by_recv_rate) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // 连续两次 late 主导降速：10MB → 5MB → 2.5MB
    rep(est, 10, 0.5);
    rep(est, 10, 0.5);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 / 4);
    // 恢复爬升受对端接收速率约束：btl ≤ max(btl, recv_rate×1.2) = 3.6MB
    rep(est, 10, 0.0, 0.0, 0.0, false, true, false, 3.0 * 1024 * 1024);
    CHECK_EQ(est.btl_bw_bps(), 3ULL * 1024 * 1024 * 12 / 10); // 3.6MB
    // 令牌受限时不约束（recv_rate ≈ 令牌速率，约束会自锁）→ 正常 ×1.5
    rep(est, 10, 0.0, 0.0, 0.0, true, true, false, 1.0 * 1024 * 1024);
    CHECK_EQ(est.btl_bw_bps(), 3ULL * 1024 * 1024 * 12 / 10 * 3 / 2); // 5.4MB
}

TEST_CASE(bandwidth_pacer_limited_vetoes_congestion) {
    BandwidthEstimator est(10 * 1024 * 1024);
    rep(est, 10, 0.0); // 恢复台阶 1
    // 本地限速中（pacer_limited）：P50 高是本地制造，不判拥塞（防崩底死锁）
    rep(est, 100, 0.0, 0.0, 0.0, true, true);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024); // 未被减半
    CHECK_EQ(est.fec_probe_extra(), 0U);             // 走台阶 2：FEC 探测移除
    CHECK(!est.congested());
}

TEST_CASE(bandwidth_btl_never_drops_below_min) {
    BandwidthEstimator est(1024 * 1024);
    for (int i = 0; i < 20; ++i) {
        rep(est, 10, 0.5);
    }
    CHECK_EQ(est.btl_bw_bps(), 12500ULL); // 100kbps 下限
}

TEST_CASE(bandwidth_report_timeout_one_shot) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // 报告持续收不到（链路严重卡顿/断流）：×0.5 单次降
    est.on_report_timeout();
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 / 2);
    // 同段停滞重复调用不叠加（one-shot）
    est.on_report_timeout();
    est.on_report_timeout();
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 / 2);
    // 下限防打穿
    BandwidthEstimator small(1024);
    small.on_report_timeout();
    CHECK_EQ(small.btl_bw_bps(), 12500ULL);
    small.on_report_timeout();
    CHECK_EQ(small.btl_bw_bps(), 12500ULL);
}

TEST_CASE(bandwidth_report_timeout_resets_probe) {
    BandwidthEstimator est(10 * 1024 * 1024);
    rep(est, 10, 0.0); // 恢复台阶 1：FEC 探测冗余 2 片
    CHECK_EQ(est.fec_probe_extra(), 2U);
    est.on_report_timeout(); // 停滞降速：重置台阶与 FEC 探测
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 / 2);
    CHECK_EQ(est.fec_probe_extra(), 0U);
}

TEST_CASE(bandwidth_report_timeout_recovery_and_retrigger) {
    BandwidthEstimator est(10 * 1024 * 1024);
    est.on_report_timeout();
    CHECK_EQ(est.btl_bw_bps(), 5ULL * 1024 * 1024);
    // 报告恢复（无拥塞信号）：恢复台阶 ×1.5 自然回升
    rep(est, 10, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 5ULL * 1024 * 1024 * 3 / 2); // 7.5MB
    rep(est, 10, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024);         // 封顶种子 10MB
    // 报告再次停滞：标志已清除，可再次单次降速（10MB/2 = 5MB）
    est.on_report_timeout();
    CHECK_EQ(est.btl_bw_bps(), 5ULL * 1024 * 1024);
    est.on_report_timeout(); // 同段仍不叠加
    CHECK_EQ(est.btl_bw_bps(), 5ULL * 1024 * 1024);
}

TEST_CASE(bandwidth_seed_clamp_on_probe) {
    BandwidthEstimator est(30 * 1024 * 1024); // 种子 30Mbps
    // probe 起步校准：btl 不超实测链路（4Mbps → 500KB/s）
    est.set_seed_and_clamp(4 * 1024 * 1024);
    CHECK_EQ(est.btl_bw_bps(), 4ULL * 1024 * 1024 / 8);
    // 只校准一次：后续 probe 不生效（种子不被锁定，恢复爬升上限仍为配置种子）
    est.set_seed_and_clamp(64 * 1024 * 1024);
    CHECK_EQ(est.btl_bw_bps(), 4ULL * 1024 * 1024 / 8);
    // 恢复爬升上限仍是配置种子 30MB（种子未锁死）
    rep(est, 10, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 4ULL * 1024 * 1024 / 8 * 3 / 2);
    // probe=0 不校准
    BandwidthEstimator e2(5 * 1024 * 1024);
    e2.set_seed_and_clamp(0);
    CHECK_EQ(e2.btl_bw_bps(), 5ULL * 1024 * 1024);
}

TEST_CASE(bandwidth_ack_only_tracks_rtt) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // 投递率样本不再用于估计：大样本不得提升 btl
    est.on_ack(100 * 1024 * 1024, 10ms);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024);
    CHECK_EQ(est.rtt().count(), 10000);
}

TEST_CASE(bandwidth_rtt_smoothing) {
    BandwidthEstimator est(1);
    for (int i = 0; i < 3; ++i) est.on_ack(0, 100ms);
    CHECK_EQ(est.rtt().count(), 100000); // 100ms = 100000us
    est.on_ack(0, 300ms);
    CHECK_EQ(est.rtt().count(), 125000); // (100000*7 + 300000)/8
}

TEST_CASE(bandwidth_zero_rtt_ignored) {
    BandwidthEstimator est(1024 * 1024);
    est.on_ack(1024, 0s);
    CHECK_EQ(est.rtt().count(), 0);
    CHECK_EQ(est.btl_bw_bps(), 1024ULL * 1024);
}

TEST_CASE(bandwidth_bps_matches_btl_no_gain) {
    BandwidthEstimator est(1024 * 1024);
    rep(est, 10, 0.03); // late 主导柔表 ×0.90
    CHECK_EQ(est.bytes_per_second(), est.btl_bw_bps());
}

TEST_CASE(bandwidth_app_limited_stays_false) {
    BandwidthEstimator est(1024 * 1024);
    rep(est, 10, 0.0);
    CHECK(!est.app_limited_state()); // AIMD 不依赖投递率，恒不更新
}

// 关键帧突发的自引入排队归一（sent_bytes_in_window）：
// 窗口发送量按 btl 排空的中位排队时间 = bytes/(2×btl) 是帧自身传输时间，
// 观测 P50 未超预期 → 不判拥塞。
TEST_CASE(bandwidth_keyframe_burst_no_false_congestion) {
    BandwidthEstimator est(12500000);  // 100Mbps 种子
    // 报告 1：小流量建立 RTprop=5ms
    est.on_report(5, 0.0, 0.0, 0.0, 10000, false, true, false, -1.0, 20000.0);
    // 报告 2-4：500KB IDR 突发窗口，P50=25ms（q=20ms = 恰好等于自引入预期
    // 500000/(2×12.5MB/s)=20ms）→ q_eff=0 → 不拥塞
    for (int i = 0; i < 3; ++i) {
        est.on_report(25, 0.0, 0.0, 0.0, 10000, false, true, false, -1.0, 500000.0);
        CHECK(!est.congested());
        CHECK_EQ(est.btl_bw_bps(), 12500000ULL);
    }
}

// 超出发送量预期的延迟仍是真拥塞：P50 远超自引入预期 → 正常降速。
TEST_CASE(bandwidth_excess_over_expected_still_congests) {
    BandwidthEstimator est(12500000);
    est.on_report(5, 0.0, 0.0, 0.0, 10000, false, true, false, -1.0, 20000.0);
    // 500KB 窗口预期 20ms，但观测 P50=65ms → q=60、q_eff=40ms
    // EWMA：12 → 20.4（>20 拥塞，delay 软信号柔表 ×0.90）
    est.on_report(65, 0.0, 0.0, 0.0, 10000, false, true, false, -1.0, 500000.0);
    CHECK(!est.congested());  // EWMA 12ms 未到阈值
    est.on_report(65, 0.0, 0.0, 0.0, 10000, false, true, false, -1.0, 500000.0);
    CHECK(est.congested());
    CHECK(est.delay_congested());
    CHECK_EQ(est.btl_bw_bps(), 12500000ULL * 90 / 100);
}
