#pragma once
// Android 客户端总装（移植 Windows client/src/main.cpp 的调度与门控逻辑）：
//   视频消息解析（ch0 基础层 tag 0x56 / ch4 增强层 tag 0x57）、
//   单槽 AU 队列 + 顶帧拒收、pts 顺序闸、got_idr 门控、关键帧看门狗、
//   令牌桶上屏调度（上屏前按 pts 挂残差 → 合成或基础帧直出）、
//   增强层解码线程（ch4 消息 → 熵解码 → 残差 map）。
//
// 语言方针：全部 C++；Java 侧只做 UI/GL 线程/输入/AudioRecord 转发。

#include "decoder.h"
#include "renderer.h"
#include "transport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Client {
public:
    static Client& instance();

    bool start(const std::string& host, std::uint16_t port, const std::string& token);
    void stop();

    bool online() const;
    bool got_idr() const { return m_got_idr.load(); }
    Renderer& renderer() { return m_renderer; }
    // 上屏统计（UI 状态条）：out 返回 [shown_total, shown_with_residual, enh_idr,
    //                              jank_rate_‰, grace_ms]
    void stats(std::uint64_t out[5]) {
        out[0] = m_shown_total.load();
        out[1] = m_shown_with_res.load();
        out[2] = m_enh_got_idr.load() ? 1 : 0;
        out[3] = (std::uint64_t)m_jank_rate_x1000.load();
        out[4] = (std::uint64_t)m_grace_cur.load();
    }

    // ---- 输入（Java 线程）----
    void send_touch(std::uint8_t action, float x, float y);
    void send_key(std::uint8_t action, std::int32_t keycode);
    void send_text(const std::string& utf8);
    void send_scroll(float x, float y, float dy);
    void send_set_format(std::uint8_t mode);
    // 上屏 grace 窗口（ms，等增强层赶上；0=立即上屏基础帧）
    void set_grace_ms(int ms) { m_scheduler.set_grace_ms(ms); }
    // ---- 麦克风 PCM（AudioRecord 线程）----
    void send_audio(const std::uint8_t* data, std::size_t size);

    // ---- Java MediaCodec 解码路径（华为 EMUI 上 NDK AImageReader 输出面
    // configure 失败 -10000 的兜底；buffer 模式各厂商兼容）----
    // JNI 层注册：按 layer 喂 AU 的 Java 回调（返回 false = 解码器忙/未就绪）
    struct JavaDecFeeder {
        std::function<bool(const std::uint8_t* au, std::size_t size,
                           std::uint64_t pts_ms, int flags)> feed;
    };
    void set_java_decoder(int layer, JavaDecFeeder feeder);
    // Java 解码输出（VideoDec 回调线程）：组装 VideoFrame 后排产
    void on_java_decoded(int layer, std::uint64_t pts_ms, int flags,
                         const std::uint8_t* y, int y_stride, int y_ps,
                         const std::uint8_t* u, int u_stride, int u_ps,
                         const std::uint8_t* v, int v_stride, int v_ps,
                         int w, int h);

private:
    Client() = default;

    void on_message(tight::Bytes payload);   // tight 线程：ch0/ch4 分发
    void on_data(tight::Bytes payload);      // DEVICE_INFO
    void on_state(bool online);
    void decode_loop();
    void enh_loop();

    // 上屏前挂残差（调度器钩子）：命中同 pts 残差 → has_residual
    void attach_residual(VideoFrame& f);

    // AU 解码 FIFO（保序全解码，参考链不断）：帧一律提交解码，上屏丢弃交给
    // DisplayScheduler（解码后丢帧不断链）。容量是病态积压（解码器卡死）的
    // 安全阀：超限才丢最旧 + 请求关键帧（正常硬解速率下不会触达）。
    struct AuItem {
        tight::Bytes au;
        bool idr = false;
        bool ycocg = false;
        bool single = false;
        std::uint64_t pts_ms = 0;
    };
    struct FrameQueue {
        static constexpr std::size_t kCap = 8;
        std::mutex mtx;
        std::condition_variable cv;
        std::deque<AuItem> items;
        bool quit = false;
        // 返回 true = 积压超限丢了最旧帧（参考链断，调用方须门控等新 IDR）
        bool push(AuItem item) {
            bool dropped;
            {
                std::lock_guard<std::mutex> lk(mtx);
                dropped = items.size() >= kCap && !quit;
                if (dropped) items.pop_front();
                items.push_back(std::move(item));
            }
            cv.notify_one();
            return dropped;
        }
        bool pop(AuItem& out) {
            std::unique_lock<std::mutex> lk(mtx);
            cv.wait(lk, [&] { return !items.empty() || quit; });
            if (quit) return false;
            out = std::move(items.front());
            items.pop_front();
            return true;
        }
        void stop() {
            {
                std::lock_guard<std::mutex> lk(mtx);
                quit = true;
            }
            cv.notify_all();
        }
        // 重连复位（quit 一旦置位 pop 永远返回 false——断开重连后画面卡死的根因）
        void reset() {
            std::lock_guard<std::mutex> lk(mtx);
            items.clear();
            quit = false;
        }
    };

    // 令牌桶上屏调度（移植 Windows DisplayScheduler）：每 1/display_fps 发 1 令牌，
    // 上限 cap = max_queue+1；有令牌立即上屏，无令牌排队，队满丢最旧留最新。
    // layer 增强语义（§3.4）：帧入队后留 grace 窗口等增强层赶上（赶上→合成上屏，
    // 赶不上→窗口到就上屏基础帧）。grace 是固定附加延迟（仅增强流活跃时有意义）。
    class DisplayScheduler {
    public:
        DisplayScheduler(int max_queue, double display_fps)
            : m_max_queue(max_queue > 0 ? max_queue : 1),
              m_period(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                  std::chrono::duration<double>(1.0 / display_fps))) {}

        void start(Client* owner) {
            m_owner = owner;
            m_running = true;
            m_thread = std::thread([this] { tick_loop(); });
        }
        void stop() {
            m_running = false;
            if (m_thread.joinable()) m_thread.join();
        }
        // 重连复位：清掉旧会话残留的已解码帧
        void reset() {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_queue.clear();
            m_tokens = 1;
            m_last_show_at = std::chrono::steady_clock::now();
        }
        void set_grace_ms(int ms) {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_grace = std::chrono::milliseconds(ms);
        }
        // 解码线程调用：有新解码帧
        void push(VideoFrame f);
    private:
        void tick_loop();
        Client* m_owner = nullptr;
        int m_max_queue;
        std::chrono::steady_clock::duration m_period;
        std::chrono::steady_clock::duration m_grace{std::chrono::milliseconds(150)};
        struct Queued {
            VideoFrame frame;
            std::chrono::steady_clock::time_point enqueued_at;
        };
        std::mutex m_mtx;
        std::deque<Queued> m_queue;
        int m_tokens = 1;
        std::atomic<bool> m_running{false};
        std::thread m_thread;
        // 最近一次上屏时刻（饿死兜底判据）：队列里没有成熟帧且画面已停滞
        // ≥ grace → 提前上屏最新基础帧（模糊 base 比冻屏好）
        std::chrono::steady_clock::time_point m_last_show_at =
                std::chrono::steady_clock::now();
    };

    TransportGlue* m_transport = nullptr;  // unique_ptr 持有
    std::unique_ptr<TransportGlue> m_transport_holder;
    Renderer m_renderer;
    FrameQueue m_queue;
    DisplayScheduler m_scheduler{8, 60.0};  // grace 窗口需要多个槽位

    std::thread m_decode_thread;
    std::thread m_enh_thread;
    std::thread m_kf_watchdog;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_got_idr{false};
    std::atomic<std::uint64_t> m_last_fed_pts{0};

    // ---- 增强层（§3.4）----
    // ch4 消息队列（tight 线程只入队，熵解码在工作线程）
    std::mutex m_enh_mtx;
    std::condition_variable m_enh_cv;
    std::deque<tight::Bytes> m_enh_queue;  // cap 2，满则丢最旧
    // 残差 map：pts_ms → {res_y, res_uv}（熵解码完成即入；上屏挂取后移除）
    std::mutex m_resid_mtx;
    struct Residual {
        std::vector<std::uint8_t> y;   // encW×H
        std::vector<std::uint8_t> uv;  // encW×H/2（NV12 交错）
    };
    std::map<std::uint64_t, Residual> m_resid;
    std::atomic<std::uint64_t> m_last_display_pts{0};
    // 上屏统计（合成命中率观测，无 logcat 时经 UI 状态条可见）
    std::atomic<std::uint64_t> m_shown_total{0};
    std::atomic<std::uint64_t> m_shown_with_res{0};
    // 当前编码帧尺寸（增强残差解码需要；来自最近基础帧）
    std::atomic<int> m_enc_w{0};
    std::atomic<int> m_enc_h{0};
    // 增强 H.264 流（kind=0x02）门控：丢帧/乱序断链 → 等增强 IDR（并发 0x07）
    std::atomic<bool> m_enh_got_idr{false};
    std::atomic<std::uint64_t> m_last_enh_pts{0};
    std::atomic<std::int64_t> m_last_enh_kf_req{0};  // 0x07 节流（1.5s）
    // Java 解码器（native NDK 路径不可用时的主路径）：layer 0=基础 1=增强
    JavaDecFeeder m_java_dec[2];
    std::atomic<bool> m_java_dec_ready{false};

    // ---- grace 窗口 P50 自适应（只覆盖偶发延迟，防退化成永远基础帧上屏）----
    // 记录基础帧（ch0）到达时刻；增强帧（ch4）到达时算滞后 lag，滑窗 120 个
    // 样本取 P50，grace = clamp(P50 + 30ms 余量, 0, 300ms)
    std::mutex m_lag_mtx;
    std::map<std::uint64_t, std::chrono::steady_clock::time_point> m_base_arrival;
    std::deque<std::int64_t> m_enh_lag_ms;
    std::atomic<int> m_grace_cur{150};
    void note_base_arrival(std::uint64_t pts_ms);
    void note_enh_arrival(std::uint64_t pts_ms);

    // ---- 卡顿率统计（attach_residual 是上屏唯一出口）----
    // 卡顿事件（两条件同时满足）：动态惯性——上屏间隔 > 前 3 帧平均间隔的 2 倍；
    // 绝对感知——间隔 > 84ms（低于 24fps 的流畅度下限）。
    // 排除画面静止：间隔期间须有基础帧到达才算管线卡顿（静止期无新帧产生）。
    // 卡顿率 = Σ 单次卡顿耗时（卡顿帧的上屏间隔）/ 测试总时长 × 100%。
    std::mutex m_jank_mtx;
    std::deque<std::int64_t> m_gaps_ms;         // 上屏间隔滑窗（cap 300，P50 参考）
    std::deque<std::int64_t> m_last3_gaps;      // 前 3 帧间隔（动态惯性判据）
    std::int64_t m_last_show_ms = 0;
    std::int64_t m_first_show_ms = 0;
    std::int64_t m_gap_p50_ms = 33;             // 滚动 P50（仅参考显示）
    std::int64_t m_jank_window_start_ms = 0;
    std::int64_t m_win_gap_ms = 0;              // 窗口内上屏间隔总和（窗口时长）
    int m_win_shown = 0, m_win_jank = 0;
    std::int64_t m_win_jank_ms = 0;             // 窗口内卡顿耗时合计
    std::int64_t m_cum_jank_ms = 0;             // 累计卡顿耗时
    std::atomic<int> m_jank_rate_x1000{0};      // 最近窗口卡顿率 ‰（UI 显示）
    std::atomic<std::int64_t> m_last_base_arrival_ms{0};
    void note_show();                            // 上屏时调用（attach_residual 内）
    void request_enh_keyframe();
    void store_residual(std::uint64_t pts_ms, std::vector<std::uint8_t> y,
                        std::vector<std::uint8_t> uv);
    // 断链（基础/增强任一侧）时清空残差 map：断链期算出的残差与重建链不一致，
    // 挂上就会闪色（红色闪屏的根因）
    void clear_residuals(const char* why);
};
