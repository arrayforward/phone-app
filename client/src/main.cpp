// tightcast-client：Windows PC 客户端入口。
// 职责：命令行解析、窗口/传输/解码/音频模块串接、GUI 消息循环。

#include "audio.h"
#include "commands.h"
#include "decoder.h"
#include "renderer.h"
#include "transport_glue.h"
#include "vpl_decoder.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <deque>

namespace {

void usage() {
    std::fprintf(stderr,
        "tightcast-client - phone screen mirror / remote control client\n"
        "usage:\n"
        "  tightcast-client <phone_ip> [--port 8800] [--token tightcast] [--no-audio]\n"
        "       [--max-queue 2]      显示调度冗余队列深度 n（令牌桶上屏）\n"
        "       [--display-fps 30]   上屏节拍（令牌发放速率，建议与编码 fps 一致）\n");
}

struct Args {
    std::string   host;
    std::uint16_t port = 8800;
    std::string   token = "tightcast";
    bool          no_audio = false;
    int           max_queue = 1;        // 显示冗余队列默认 1（远程操控低时延优先）
    double        display_fps = 60.0;   // 上屏限速默认 60fps（采集源屏幕刷新率）
};

bool parse_args(int argc, char** argv, Args& out) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) {
            out.port = (std::uint16_t)std::atoi(argv[++i]);
        } else if (a == "--token" && i + 1 < argc) {
            out.token = argv[++i];
        } else if (a == "--max-queue" && i + 1 < argc) {
            out.max_queue = std::atoi(argv[++i]);
        } else if (a == "--display-fps" && i + 1 < argc) {
            out.display_fps = std::atof(argv[++i]);
        } else if (a == "--no-audio") {
            out.no_audio = true;
        } else if (a == "--help" || a == "-h") {
            return false;
        } else if (a.rfind("--", 0) == 0) {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            return false;
        } else if (out.host.empty()) {
            out.host = a;
        } else {
            return false;
        }
    }
    return !out.host.empty();
}

// 视频帧队列：单槽（来新帧覆盖旧帧，零积压低时延）
// push 返回是否顶掉了未被解码的旧帧：顶掉 P 帧会断参考链导致花屏，
// 调用方须据此请求关键帧并暂停送解码直到新 IDR 到达。
struct FrameQueue {
    std::mutex mtx;
    std::condition_variable cv;
    tight::Bytes pending;
    std::chrono::steady_clock::time_point queued_at;  // 入队时刻（排队时延统计用）
    bool pending_ycocg = false;  // 入队帧的打包模式（视频消息头 flags bit1）
    bool has = false;
    bool quit = false;

    bool push(tight::Bytes frame, bool ycocg) {
        bool dropped;
        {
            std::lock_guard<std::mutex> lk(mtx);
            dropped = has && !quit;  // 槽位里还有未消费帧 = 被顶掉丢弃
            pending = std::move(frame);
            pending_ycocg = ycocg;
            queued_at = std::chrono::steady_clock::now();
            has = true;
        }
        cv.notify_one();
        return dropped;
    }
    // 槽位是否有未被消费的帧（顶帧预判：有则新 P 帧应拒收而非覆盖）
    bool occupied() {
        std::lock_guard<std::mutex> lk(mtx);
        return has && !quit;
    }
    bool pop(tight::Bytes& out, std::chrono::steady_clock::time_point* qat = nullptr,
             bool* ycocg = nullptr) {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk, [&] { return has || quit; });
        if (quit) return false;
        out = std::move(pending);
        if (qat) *qat = queued_at;
        if (ycocg) *ycocg = pending_ycocg;
        has = false;
        return true;
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lk(mtx);
            quit = true;
        }
        cv.notify_all();
    }
};

// 门控转换日志（排查冻结）：gate 状态变化时打一行
void gate_log(std::atomic<bool>& gate, bool open, const char* why) {
    if (gate.exchange(open) != open)
        std::fprintf(stderr, "[gate] %s -> %s\n", why, open ? "open" : "CLOSED");
}

// DEVICE_INFO（protocol 第 6 节）：0x01 | w u16be | h u16be | name_len u8 | name
bool parse_device_info(const tight::Bytes& d, int& w, int& h, std::string& name) {
    if (d.size() < 6 || d[0] != 0x01) return false;
    w = (d[1] << 8) | d[2];
    h = (d[3] << 8) | d[4];
    std::size_t n = d[5];
    if (d.size() < 6 + n) return false;
    name.assign((const char*)d.data() + 6, n);
    return true;
}

// 显示调度器：令牌桶上屏限速 + 可配置冗余队列（n）。
// 每个显示周期（1/display_fps）发 1 个令牌，上限 cap = n+1——卡顿期令牌
// 积攒（欠帧），恢复后欠的帧可补屏。解码帧到达：有令牌→立即上屏消耗一个；
// 无令牌且队列 <n→排队；队列满→丢弃（帧已解码，丢弃不影响参考链）。
// 令牌/队列由 tick 线程与 push（解码线程）共用一把锁；update_frame 锁外调用
// （其内部是 mutex 拷贝 + InvalidateRect，快且不阻塞）。
class DisplayScheduler {
public:
    DisplayScheduler(int max_queue, double display_fps)
        : m_max_queue(std::max(1, max_queue)),
          m_period(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
              std::chrono::duration<double>(1.0 / display_fps))) {}

    void start(Renderer* r) {
        m_renderer = r;
        m_running = true;
        m_thread = std::thread([this] { tick_loop(); });
    }
    void stop() {
        m_running = false;
        if (m_thread.joinable()) m_thread.join();
    }

    std::uint64_t dropped_total() const { return m_dropped.load(); }

    // 解码线程调用：有新解码帧
    void push(VideoFrame f) {
        VideoFrame show;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_tokens > 0) {
                --m_tokens;
                show = std::move(f);
            } else if ((int)m_queue.size() < m_max_queue) {
                m_queue.push_back(std::move(f));
            } else {
                // 队列满：丢最旧留最新（最新帧必须最终上屏——突发结束画面
                // 静止后不会再有新帧，丢了最新帧端侧就永远停在旧状态）
                m_queue.pop_front();
                m_queue.push_back(std::move(f));
                ++m_dropped;
            }
        }
        if (show.width > 0) m_renderer->update_frame(std::move(show));
    }

private:
    void tick_loop() {
        auto next = std::chrono::steady_clock::now() + m_period;
        while (m_running.load()) {
            std::this_thread::sleep_until(next);
            if (!m_running.load()) break;
            next += m_period;
            VideoFrame show;
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                if (m_tokens < m_max_queue + 1) ++m_tokens;  // 欠帧积攒
                if (!m_queue.empty() && m_tokens > 0) {
                    --m_tokens;
                    show = std::move(m_queue.front());
                    m_queue.pop_front();
                }
            }
            if (show.width > 0) m_renderer->update_frame(std::move(show));
        }
    }

    Renderer* m_renderer = nullptr;
    int m_max_queue;
    std::chrono::steady_clock::duration m_period;

    std::mutex m_mtx;
    std::deque<VideoFrame> m_queue;
    int m_tokens = 1;
    std::atomic<bool> m_running{false};
    std::atomic<std::uint64_t> m_dropped{0};
    std::thread m_thread;
};

} // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) {
        usage();
        return argc > 1 ? 1 : 0;
    }

    Renderer renderer;
    if (!renderer.create(1280, 720, "tightcast")) {
        std::fprintf(stderr, "[main] create window failed\n");
        return 1;
    }

    FrameQueue queue;
    std::atomic<bool> got_idr{false};
    // 显示调度器：令牌桶上屏限速 + 冗余队列（--max-queue/--display-fps）
    DisplayScheduler scheduler(args.max_queue, args.display_fps);
    // 解码顺序闸：最近送入解码的帧 pts（编码时间戳单调递增）。ARQ 重传会
    // 让丢分片的帧晚于后续帧重组完成——迟到帧若照常送解码则解码顺序断裂
    // （马赛克），故 pts 不增的旧帧按丢帧处理。
    std::atomic<std::uint64_t> last_fed_pts{0};
    std::atomic<std::uint64_t> reorder_drops{0};
    // 临时 E2E 时延探针：发触控命令打点，下一帧视频消息到达时输出毫秒差
    std::atomic<std::int64_t> e2e_probe_ms{-1};

    TransportGlue transport(
        {args.host, args.port, args.token},
        // 视频帧（message 回调，通道 0 语义）：tag 0x56 + flags u8 + pts u64be + Annex-B AU
        [&](tight::Bytes payload) {
            // （tag 用于避开 tight 内部保留首字节 0x01/0x02/0x03，见 protocol §3）
            if (payload.size() < 11 || payload[0] != 0x56) return;
            std::uint8_t payload_flags = payload[1];  // bit0=IDR bit1=YCoCg（§3.2）
            bool idr = (payload_flags & 0x01) != 0;
            // E2E 探针：触控命令后的首帧到达时间
            std::int64_t stamp = e2e_probe_ms.exchange(-1);
            if (stamp > 0) {
                std::int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                std::fprintf(stderr, "[e2e] command→frame-arrived %lldms\n",
                             (long long)(now_ms - stamp));
            }
            // pts_ms（大端 u64，编码时间戳）：解码顺序闸用
            std::uint64_t pts_ms = 0;
            for (int i = 0; i < 8; ++i)
                pts_ms = (pts_ms << 8) | payload[2 + i];
            // 首个 IDR 之前不送解码器；丢帧（队列顶帧/重组失败/乱序）后
            // 同样等新 IDR——P 帧参考链断裂时继续送解码必然花屏
            if (!got_idr.load() && !idr) return;
            // 乱序检测：pts 不增 = ARQ 重传迟到的旧帧（它之后的帧已送解码，
            // 参考链已断）——按丢帧处理，请求关键帧恢复
            if (pts_ms <= last_fed_pts.load()) {
                gate_log(got_idr, false, "reorder");
                reorder_drops.fetch_add(1);
                transport.request_keyframe();
                return;
            }
            // 顶帧拒收：槽里有未解码帧时新 P 帧直接拒收——顶掉旧帧再送解码
            // 会断参考链（花屏）；保留旧帧正常解码，门控等新 IDR
            if (!idr && queue.occupied()) {
                gate_log(got_idr, false, "decode-busy");
                transport.request_keyframe();
                return;
            }
            payload.erase(payload.begin(), payload.begin() + 10);
            queue.push(std::move(payload), (payload_flags & 0x02) != 0);
            last_fed_pts.store(pts_ms);
            if (idr) gate_log(got_idr, true, "idr");
        },
        // data 通道（DEVICE_INFO）
        [&](tight::Bytes payload) {
            int w, h;
            std::string name;
            if (parse_device_info(payload, w, h, name)) {
                std::fprintf(stderr, "[main] DEVICE_INFO: %dx%d %s\n", w, h, name.c_str());
                renderer.set_device_info(w, h, name);
            }
        },
        // 连接状态
        [&](bool online) {
            std::fprintf(stderr, "[main] %s\n", online ? "online" : "disconnected");
            gate_log(got_idr, false, "link");  // 重连后等待新 IDR（REQ_KEYFRAME 已由 transport 发出）
            renderer.set_connected(online);
        });
    // tight 重组失败（视频消息丢失）→ 暂停送解码直到新 IDR（防花屏）
    transport.set_video_loss_callback([&] { gate_log(got_idr, false, "loss"); });

    // 输入 → 控制命令（GUI 线程回调，tight 线程安全可直接发）
    Renderer::Handlers handlers;
    handlers.on_touch = [&](std::uint8_t action, float x, float y) {
        e2e_probe_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        transport.send_command(tc::build_touch(action, x, y));
    };
    handlers.on_scroll = [&](float x, float y, float dy) {
        transport.send_command(tc::build_scroll(x, y, dy));
    };
    handlers.on_key = [&](std::uint8_t action, std::int32_t kc) {
        transport.send_command(tc::build_key(action, kc));
    };
    handlers.on_text = [&](const std::string& s) {
        transport.send_command(tc::build_text(s));
    };
    renderer.set_handlers(std::move(handlers));
    scheduler.start(&renderer);

    // 解码线程：COM/MF 按线程初始化，全部在此线程内。
    // 解码器选择：优先 Intel oneVPL 硬解，init 失败回退 Media Foundation。
    std::thread decode_thread([&] {
        VplDecoder vpl;
        H264Decoder mf;
        bool use_vpl = false;
        bool dec_ready = false;
        // 临时排查：FORCE_MF=1 跳过 VPL 直走 MF 路径
        if (getenv("FORCE_MF") == nullptr && vpl.init()) {
            use_vpl = dec_ready = true;
            std::fprintf(stderr, "[main] decoder: Intel oneVPL (hardware)\n");
        } else {
            std::fprintf(stderr, "[main] oneVPL unavailable, falling back to Media Foundation\n");
            if (mf.init()) {
                dec_ready = true;
                std::fprintf(stderr, "[main] decoder: Media Foundation\n");
            }
        }
        if (!dec_ready) {
            std::fprintf(stderr, "[main] decoder init failed, video disabled\n");
        }
        tight::Bytes au;
        VideoFrame frame;
        // 轻量指标：解码耗时（均值/峰值）、解码帧率、网络 P50、入队→解码
        // 排队时延（每 2s 打一次）
        using clk = std::chrono::steady_clock;
        auto stat_start = clk::now();
        std::uint64_t stat_dec_us = 0, stat_dec_max_us = 0;
        std::uint64_t stat_q_us = 0;
        std::size_t stat_in = 0, stat_out = 0;
        std::uint64_t stat_drop_last = 0, stat_reorder_last = 0;
        std::chrono::steady_clock::time_point qat;
        bool au_ycocg = false;
        while (queue.pop(au, &qat, &au_ycocg)) {
            auto t0 = clk::now();
            stat_q_us += (std::uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                             t0 - qat).count();
            bool ok = use_vpl ? vpl.decode(au.data(), au.size(), frame)
                              : mf.decode(au.data(), au.size(), frame);
            auto t1 = clk::now();
            ++stat_in;
            std::uint64_t us =
                (std::uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0)
                    .count();
            stat_dec_us += us;
            if (us > stat_dec_max_us) stat_dec_max_us = us;
            if (!ok) continue;
            ++stat_out;
            frame.ycocg = au_ycocg;  // 打包模式随帧传给渲染器（shader 双公式）
            // 交给显示调度器：令牌够立即上屏，否则按冗余 n 排队/丢弃
            scheduler.push(std::move(frame));
            frame = VideoFrame{};
            if (t1 - stat_start >= std::chrono::seconds(2)) {
                double secs = std::chrono::duration<double>(t1 - stat_start).count();
                std::uint64_t drops = scheduler.dropped_total();
                std::uint64_t reords = reorder_drops.load();
                std::fprintf(stderr,
                             "[stats] decode_fps=%.1f decode_avg=%.1fms max=%.1fms "
                             "queue_avg=%.1fms net_p50=%ums disp_drop=%llu reorder=%llu "
                             "(in=%zu out=%zu)\n",
                             stat_out / secs, stat_out ? stat_dec_us / 1000.0 / stat_in : 0.0,
                             stat_dec_max_us / 1000.0,
                             stat_in ? stat_q_us / 1000.0 / stat_in : 0.0,
                             transport.peer_p50_ms(),
                             (unsigned long long)(drops - stat_drop_last),
                             (unsigned long long)(reords - stat_reorder_last),
                             stat_in, stat_out);
                stat_drop_last = drops;
                stat_reorder_last = reords;
                stat_start = t1;
                stat_dec_us = stat_dec_max_us = 0;
                stat_q_us = 0;
                stat_in = stat_out = 0;
            }
        }

        if (use_vpl) vpl.shutdown();
        else mf.shutdown();
    });

    // 端口 18800 被占用时（僵尸实例残留/释放延迟）重试而不是立刻退出：
    // Windows 结束进程后 UDP 端口释放有秒级延迟，直接退出会让用户以为挂了
    int tries = 0;
    while (!transport.start()) {
        if (++tries > 20) {
            std::fprintf(stderr, "[main] transport start failed after retries\n");
            // 直接进程退出：不走 decode_thread.join()/decoder.shutdown() 的优雅清理——
            // 解码器 shutdown（VPL/MF）可能挂起，导致进程僵死、占用 18800 端口，
            // 后续客户端全部启动失败。交给 OS 回收即可。
            std::fflush(stderr);
            _exit(1);
        }
        std::fprintf(stderr, "[main] transport start failed, retry %d/20...\n", tries);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // 关键帧看门狗：收到首个 IDR 前每 1.5s 重发 REQ_KEYFRAME
    // （IDR 丢失后 loss 回调之外的一层兜底；拿到 IDR 即静默）
    std::atomic<bool> kf_watchdog_run{true};
    std::thread kf_watchdog([&] {
        while (kf_watchdog_run.load()) {
            for (int i = 0; i < 15 && kf_watchdog_run.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!kf_watchdog_run.load()) break;
            if (transport.online() && !got_idr.load()) transport.request_keyframe();
        }
    });

    // 麦克风声音注入（通道 1），--no-audio 关闭
    AudioCapture audio;
    if (!args.no_audio) {
        audio.start([&](const std::uint8_t* data, std::size_t size) {
            transport.send_audio(data, size);
        });
    }

    // GUI 消息循环
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    audio.stop();
    kf_watchdog_run = false;
    kf_watchdog.join();
    transport.stop();
    queue.stop();
    decode_thread.join();
    scheduler.stop();
    return 0;
}
