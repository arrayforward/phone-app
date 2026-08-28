#include "client.h"

#include "layered/entropy.h"
#include "layered/residual.h"

#include <android/log.h>
#include <algorithm>
#include <cstring>

#define TAG "tightcast-client"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

using namespace std::chrono;

namespace ClientLog {
    // native 事件 → Java Log2File（华为 ROM 抑制应用 logcat）
    void hook(const char* msg);
}

#define CLOG(...) do { \
    char _buf[192]; \
    std::snprintf(_buf, sizeof(_buf), __VA_ARGS__); \
    ClientLog::hook(_buf); \
    LOGI("%s", _buf); \
} while (0)

namespace {

// ---- 控制命令构造（协议 §5，全部大端；与 Windows client/src/commands.h 同构）----
void put_u32be(tight::Bytes& v, std::uint32_t x) {
    v.push_back((std::uint8_t)(x >> 24));
    v.push_back((std::uint8_t)(x >> 16));
    v.push_back((std::uint8_t)(x >> 8));
    v.push_back((std::uint8_t)x);
}

void put_f32be(tight::Bytes& v, float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    put_u32be(v, u);
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

}  // namespace

Client& Client::instance() {
    static Client c;
    return c;
}

bool Client::start(const std::string& host, std::uint16_t port, const std::string& token) {
    if (m_running.exchange(true)) return true;  // 已在运行
    m_got_idr = false;
    m_last_fed_pts = 0;
    m_last_display_pts = 0;
    m_enc_w = 0;
    m_enc_h = 0;
    m_enh_got_idr = false;
    m_last_enh_pts = 0;
    m_last_enh_kf_req = 0;
    {
        std::lock_guard<std::mutex> lk(m_resid_mtx);
        m_resid.clear();
    }
    {
        std::lock_guard<std::mutex> lk(m_lag_mtx);
        m_base_arrival.clear();
        m_enh_lag_ms.clear();
    }
    m_queue.reset();      // 断开重连关键复位（quit 置位后 pop 永 false 的卡死根因）
    m_scheduler.reset();  // 清掉旧会话残留的已解码帧

    m_transport_holder = std::make_unique<TransportGlue>(
        TransportGlue::Options{host, port, token},
        [this](tight::Bytes p) { on_message(std::move(p)); },
        [this](tight::Bytes p) { on_data(std::move(p)); },
        [this](bool online) { on_state(online); });
    m_transport = m_transport_holder.get();
    m_transport->set_video_loss_callback([this] {
        // ch0 基础层重组失败 → 暂停送解码直到新 IDR（防花屏）
        if (m_got_idr.exchange(false)) CLOG("[gate] loss -> CLOSED");
        clear_residuals("base-loss");  // 断链前算出的残差不得再配对
    });
    m_transport->set_enh_loss_callback([this] {
        // ch4 增强层重组失败 → 增强参考链断裂：门控等增强 IDR + 请求恢复
        if (m_enh_got_idr.exchange(false)) CLOG("[enh-gate] loss -> CLOSED");
        clear_residuals("enh-loss");   // 断链后的残差是垃圾（闪色来源）
        request_enh_keyframe();
    });

    if (!m_transport->start()) {
        LOGE("transport start failed");
        m_transport_holder.reset();
        m_transport = nullptr;
        m_running = false;
        return false;
    }

    m_decode_thread = std::thread([this] { decode_loop(); });
    m_enh_thread = std::thread([this] { enh_loop(); });
    m_scheduler.start(this);
    // 关键帧看门狗：收到首个 IDR 前每 1.5s 重发 REQ_KEYFRAME
    m_kf_watchdog = std::thread([this] {
        while (m_running.load()) {
            for (int i = 0; i < 15 && m_running.load(); ++i)
                std::this_thread::sleep_for(milliseconds(100));
            if (!m_running.load()) break;
            if (m_transport && m_transport->online() && !m_got_idr.load())
                m_transport->request_keyframe();
        }
    });
    LOGI("started -> %s:%u", host.c_str(), (unsigned)port);
    return true;
}

void Client::stop() {
    if (!m_running.exchange(false)) return;
    if (m_transport) m_transport->stop();
    m_queue.stop();
    {
        std::lock_guard<std::mutex> lk(m_enh_mtx);
        m_enh_queue.clear();
    }
    m_enh_cv.notify_all();
    if (m_decode_thread.joinable()) m_decode_thread.join();
    if (m_enh_thread.joinable()) m_enh_thread.join();
    if (m_kf_watchdog.joinable()) m_kf_watchdog.join();
    m_scheduler.stop();
    m_transport_holder.reset();
    m_transport = nullptr;
    LOGI("stopped");
}

bool Client::online() const {
    return m_transport && m_transport->online();
}

// ---- tight 线程回调 ----

void Client::on_message(tight::Bytes payload) {
    if (payload.size() < 1) return;
    if (payload[0] == 0x57) {
        // ch4 增强层（§3.4）：tight 线程只入队（cap 2，满丢最旧），熵解码在工作线程
        std::lock_guard<std::mutex> lk(m_enh_mtx);
        if (m_enh_queue.size() >= 2) m_enh_queue.pop_front();
        m_enh_queue.push_back(std::move(payload));
        m_enh_cv.notify_one();
        return;
    }
    // ch0 基础层视频帧：tag 0x56 + flags u8 + pts u64be + Annex-B AU
    if (payload.size() < 11 || payload[0] != 0x56) return;
    std::uint8_t flags = payload[1];  // bit0=IDR bit1=YCoCg bit2=single（§3）
    bool idr = (flags & 0x01) != 0;
    std::uint64_t pts_ms = 0;
    for (int i = 0; i < 8; ++i)
        pts_ms = (pts_ms << 8) | payload[2 + i];
    // 首个 IDR 之前不送解码器；丢帧后同样等新 IDR（P 帧参考链断裂续送必花屏）
    if (!m_got_idr.load() && !idr) return;
    // 乱序检测：pts 不增 = ARQ 重传迟到的旧帧——按丢帧处理
    if (pts_ms <= m_last_fed_pts.load()) {
        if (m_got_idr.exchange(false)) CLOG("[gate] reorder -> CLOSED");
        clear_residuals("base-reorder");
        if (m_transport) m_transport->request_keyframe();
        return;
    }
    // 一律提交解码（推进参考链），上屏丢弃交给 DisplayScheduler——
    // 拒收未解码的 P 帧会断参考链并引发关键帧风暴（修复前行为）
    AuItem item;
    item.idr = idr;
    item.ycocg = (flags & 0x02) != 0;
    item.single = (flags & 0x04) != 0;
    item.pts_ms = pts_ms;
    item.au.assign(payload.begin() + 10, payload.end());
    if (m_queue.push(std::move(item))) {
        // 病态积压（解码器卡死级别）才会触达：丢了解码中的最旧帧 → 断链
        if (m_got_idr.exchange(false)) CLOG("[gate] decode-overflow -> CLOSED");
        clear_residuals("base-overflow");
        if (m_transport) m_transport->request_keyframe();
    }
    m_last_fed_pts.store(pts_ms);
    m_last_base_arrival_ms.store(duration_cast<std::chrono::milliseconds>(
            steady_clock::now().time_since_epoch()).count());  // 卡顿判定用
    note_base_arrival(pts_ms);  // grace P50 自适应：基础层到达时刻
    if (idr && !m_got_idr.exchange(true)) CLOG("[gate] idr -> open");
}

void Client::on_data(tight::Bytes payload) {
    int w, h;
    std::string name;
    if (parse_device_info(payload, w, h, name)) {
        LOGI("DEVICE_INFO: %dx%d %s", w, h, name.c_str());
        m_renderer.set_device_info(w, h);
    }
}

void Client::on_state(bool online) {
    LOGI("%s", online ? "online" : "disconnected");
    if (!online) {
        if (m_got_idr.exchange(false)) CLOG("[gate] link -> CLOSED");
        if (m_enh_got_idr.exchange(false)) CLOG("[enh-gate] link -> CLOSED");
        clear_residuals("link");
    }
    m_renderer.set_connected(online);
}

// ---- 解码线程 ----

void Client::set_java_decoder(int layer, JavaDecFeeder feeder) {
    if (layer < 0 || layer > 1) return;
    m_java_dec[layer] = std::move(feeder);
    if (m_java_dec[0].feed && m_java_dec[1].feed) m_java_dec_ready = true;
}

void Client::on_java_decoded(int layer, std::uint64_t pts_ms, int flags,
                             const std::uint8_t* py, int y_stride, int y_ps,
                             const std::uint8_t* pu, int u_stride, int u_ps,
                             const std::uint8_t* pv, int v_stride, int v_ps,
                             int w, int h) {
    if (w <= 0 || h <= 0 || (w & 1) || (h & 1)) return;
    VideoFrame f;
    f.width = w;
    f.height = h;
    f.ycocg = (flags & 0x02) != 0;
    f.single = (flags & 0x04) != 0;
    f.pts_ms = pts_ms;
    f.y.resize((std::size_t)w * h);
    f.uv.resize((std::size_t)w * (h / 2));
    for (int r = 0; r < h; ++r) {
        const std::uint8_t* s = py + (std::size_t)r * y_stride;
        std::uint8_t* d = f.y.data() + (std::size_t)r * w;
        if (y_ps == 1) {
            std::memcpy(d, s, (std::size_t)w);
        } else {
            for (int c = 0; c < w; ++c) d[c] = s[(std::size_t)c * y_ps];
        }
    }
    const int cw = w / 2, ch = h / 2;
    for (int r = 0; r < ch; ++r) {
        const std::uint8_t* su = pu + (std::size_t)r * u_stride;
        const std::uint8_t* sv = pv + (std::size_t)r * v_stride;
        std::uint8_t* d = f.uv.data() + (std::size_t)r * cw * 2;
        for (int c = 0; c < cw; ++c) {
            d[c * 2] = su[(std::size_t)c * u_ps];
            d[c * 2 + 1] = sv[(std::size_t)c * v_ps];
        }
    }
    if (layer == 0) {
        m_enc_w = w;
        m_enc_h = h;
        m_scheduler.push(std::move(f));
    } else {
        m_last_enh_pts.store(pts_ms);
        store_residual(pts_ms, std::move(f.y), std::move(f.uv));
    }
}

void Client::decode_loop() {
    H264Decoder dec;
    AuItem item;
    while (m_queue.pop(item)) {
        if (m_java_dec_ready.load()) {
            // Java MediaCodec 路径（华为等 NDK AImageReader 不通的机型）
            int flags = (item.idr ? 1 : 0) | (item.ycocg ? 0x02 : 0) | (item.single ? 0x04 : 0);
            if (!m_java_dec[0].feed(item.au.data(), item.au.size(), item.pts_ms, flags)) {
                // 解码器忙/未就绪/配置失败：丢帧 → 基础层断链门控 + 请求关键帧
                // （IDR 喂失败也要兜：否则格式切换后解码器配置失败会永久黑屏——
                //  双击断流 51s 的实测根因）
                if (m_got_idr.exchange(false)) {
                    CLOG("[gate] java-dec-busy -> CLOSED (idr=%d)", (int)item.idr);
                    clear_residuals("base-java-busy");
                    if (m_transport) m_transport->request_keyframe();
                }
            }
            continue;
        }
        VideoFrame frame;
        if (!dec.decode(item.au.data(), item.au.size(), item.idr, frame)) {
            continue;
        }
        frame.ycocg = item.ycocg;
        frame.single = item.single;
        frame.pts_ms = item.pts_ms;
        m_enc_w = frame.width;   // 增强残差解码的尺寸依据
        m_enc_h = frame.height;
        m_scheduler.push(std::move(frame));
    }
    dec.shutdown();
}

void Client::DisplayScheduler::push(VideoFrame f) {
    VideoFrame show;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        bool mature = m_grace.count() <= 0;  // grace=0 才有"有令牌立即上屏"
        if (m_tokens > 0 && mature) {
            --m_tokens;
            show = std::move(f);
        } else if ((int)m_queue.size() < m_max_queue) {
            m_queue.push_back({std::move(f), std::chrono::steady_clock::now()});
        } else {
            // 队列满：丢最旧留最新（最新帧必须最终上屏）
            m_queue.pop_front();
            m_queue.push_back({std::move(f), std::chrono::steady_clock::now()});
        }
    }
    if (show.width > 0) {
        m_owner->attach_residual(show);
        m_owner->m_renderer.update_frame(std::move(show));
    }
}

void Client::DisplayScheduler::tick_loop() {
    auto next = std::chrono::steady_clock::now() + m_period;
    while (m_running.load()) {
        std::this_thread::sleep_until(next);
        if (!m_running.load()) break;
        next += m_period;
        VideoFrame show;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            auto now = std::chrono::steady_clock::now();
            if (m_tokens < m_max_queue + 1) ++m_tokens;  // 欠帧积攒
            // grace 窗口：帧成熟（入队满 grace）才可上屏——增强层赶上即合成
            if (!m_queue.empty() && m_tokens > 0
                    && now - m_queue.front().enqueued_at >= m_grace) {
                --m_tokens;
                show = std::move(m_queue.front().frame);
                m_queue.pop_front();
            } else if (!m_queue.empty()
                       && now - m_last_show_at >= m_grace) {
                // 饿死兜底（新帧断流/首个帧未成熟）：提前上屏最新的基础帧
                // （模糊的 base 比冻屏好；跳过的是已解码旧帧，不断参考链）
                if (m_tokens > 0) --m_tokens;
                show = std::move(m_queue.back().frame);
                m_queue.clear();
            }
        }
        if (show.width > 0) {
            m_last_show_at = std::chrono::steady_clock::now();
            m_owner->attach_residual(show);
            m_owner->m_renderer.update_frame(std::move(show));
        }
    }
}

// ---- 增强层配对（§3.4 上屏语义）----
// 基础帧上屏时刻：同 pts 残差已就绪 → 挂到帧上由 shader 合成；
// 未就绪 → 立即上屏低码率基础帧（不为等增强层引入额外延迟）。
// ---- 卡顿率统计 ----
// 卡顿事件 = 动态惯性（间隔 > 前 3 帧均值 × 2）∧ 绝对感知（间隔 > 84ms），
// 且间隔期间有基础帧到达（排除画面静止）。卡顿率 = Σ 卡顿耗时 / 总时长 × 100%。
void Client::note_show() {
    std::int64_t now = duration_cast<std::chrono::milliseconds>(
            steady_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lk(m_jank_mtx);
    if (m_first_show_ms == 0) {
        m_first_show_ms = now;
        m_jank_window_start_ms = now;
        m_last_show_ms = now;
        return;  // 首帧无间隔
    }
    std::int64_t gap = now - m_last_show_ms;
    m_last_show_ms = now;
    if (gap <= 0) return;

    m_gaps_ms.push_back(gap);
    if (m_gaps_ms.size() > 300) m_gaps_ms.pop_front();
    ++m_win_shown;
    m_win_gap_ms += gap;

    // 动态惯性条件：当前间隔 > 前 3 帧平均的 2 倍
    bool inertia = false;
    if (!m_last3_gaps.empty()) {
        std::int64_t sum = 0;
        for (auto g : m_last3_gaps) sum += g;
        inertia = gap > 2 * (sum / (std::int64_t)m_last3_gaps.size());
    }
    bool absolute = gap > 84;  // 绝对感知条件
    bool fed = m_last_base_arrival_ms.load() > m_last_show_ms - gap;  // 期间有帧到达
    if (inertia && absolute && fed) {
        ++m_win_jank;
        m_win_jank_ms += gap;      // 单次卡顿耗时 = 该次卡顿帧的上屏间隔
        m_cum_jank_ms += gap;
    }
    m_last3_gaps.push_back(gap);
    if (m_last3_gaps.size() > 3) m_last3_gaps.pop_front();

    if (now - m_jank_window_start_ms >= 10000) {
        if (!m_gaps_ms.empty()) {
            std::vector<std::int64_t> sorted(m_gaps_ms.begin(), m_gaps_ms.end());
            std::sort(sorted.begin(), sorted.end());
            m_gap_p50_ms = sorted[sorted.size() / 2];
        }
        // 窗口卡顿率 = Σ 卡顿耗时 / 窗口上屏总时长；累计 = Σ / （首帧至今）
        int win_rate = m_win_gap_ms > 0 ? (int)(m_win_jank_ms * 1000 / m_win_gap_ms) : 0;
        m_jank_rate_x1000 = win_rate;
        std::int64_t cum_span = now - m_first_show_ms;
        int cum_rate = cum_span > 0 ? (int)(m_cum_jank_ms * 1000 / cum_span) : 0;
        CLOG("[jank] stutter=%d.%d%% events=%d jank_ms=%lld/%lld gap_p50=%lldms cum=%d.%d%%",
             win_rate / 10, win_rate % 10, m_win_jank,
             (long long)m_win_jank_ms, (long long)m_win_gap_ms,
             (long long)m_gap_p50_ms, cum_rate / 10, cum_rate % 10);
        m_win_shown = m_win_jank = 0;
        m_win_jank_ms = 0;
        m_win_gap_ms = 0;
        m_jank_window_start_ms = now;
    }
}

void Client::attach_residual(VideoFrame& f) {
    note_show();  // 卡顿率统计（上屏唯一出口）
    std::lock_guard<std::mutex> lk(m_resid_mtx);
    m_last_display_pts.store(f.pts_ms);
    ++m_shown_total;
    if (m_shown_total % 60 == 0) {
        CLOG("[disp] shown=%llu composited=%llu resid_map=%zu grace_ok",
             (unsigned long long)m_shown_total.load(),
             (unsigned long long)m_shown_with_res.load(), m_resid.size());
    }
    auto it = m_resid.find(f.pts_ms);
    if (it == m_resid.end()) return;
    if (it->second.y.size() == (std::size_t)f.width * f.height
            && it->second.uv.size() == (std::size_t)f.width * (f.height / 2)) {
        // 残差体检：抽样 mean|sym−128|。残差应集中在 128 附近（零残差）；
        // 错配/断链的垃圾残差 mad 显著偏大——闪红嫌疑帧直接打点
        std::uint64_t acc = 0;
        std::size_t n = 0;
        const std::vector<std::uint8_t>& ry = it->second.y;
        for (std::size_t i = 0; i + 16 < ry.size(); i += 16) {
            acc += std::abs((int)ry[i] - 128);
            ++n;
        }
        double mad = n ? (double)acc / n : 0.0;
        if (mad > 24.0) {
            CLOG("[resid] LARGE mad=%.1f pts=%llu (shown #%llu)", mad,
                 (unsigned long long)f.pts_ms,
                 (unsigned long long)m_shown_total.load());
        }
        f.res_y = std::move(it->second.y);
        f.res_uv = std::move(it->second.uv);
        f.has_residual = true;
        ++m_shown_with_res;
    }
    m_resid.erase(it);
}

// ---- 增强层解码线程：ch4 消息 → 残差 map ----
// kind=0x01：Rice 无损熵编码（对照路径）；kind=0x02：H.264 直偏置 sym 硬编
// （默认路径，帧间预测 + 周期 IDR；丢帧断链 → 门控等增强 IDR + 0x07 恢复）

void Client::request_enh_keyframe() {
    // 1.5s 节流（建连初期/断链期反复触发是常态）
    std::int64_t now = duration_cast<std::chrono::milliseconds>(
            steady_clock::now().time_since_epoch()).count();
    std::int64_t last = m_last_enh_kf_req.load();
    if (now - last < 1500) return;
    if (m_last_enh_kf_req.compare_exchange_strong(last, now) && m_transport)
        m_transport->send_command({0x07});
}

// ---- grace 窗口 P50 自适应 ----

void Client::note_base_arrival(std::uint64_t pts_ms) {
    std::lock_guard<std::mutex> lk(m_lag_mtx);
    m_base_arrival[pts_ms] = steady_clock::now();
    // 容量上界：增强层丢/迟到的残留按 pts 序清最旧
    while (m_base_arrival.size() > 64) m_base_arrival.erase(m_base_arrival.begin());
}

void Client::note_enh_arrival(std::uint64_t pts_ms) {
    std::int64_t lag = -1;
    {
        std::lock_guard<std::mutex> lk(m_lag_mtx);
        auto it = m_base_arrival.find(pts_ms);
        if (it == m_base_arrival.end()) return;  // 基础帧未记录（断流期/挤出）
        lag = duration_cast<std::chrono::milliseconds>(
                steady_clock::now() - it->second).count();
        m_base_arrival.erase(it);
        m_enh_lag_ms.push_back(lag);
        if (m_enh_lag_ms.size() > 120) m_enh_lag_ms.pop_front();
    }
    // 每 30 个样本重估一次 grace：P50 + 30ms 余量，钳制 [0, 300]
    if (m_enh_lag_ms.size() >= 30 && m_enh_lag_ms.size() % 30 == 0) {
        std::vector<std::int64_t> sorted;
        {
            std::lock_guard<std::mutex> lk(m_lag_mtx);
            sorted.assign(m_enh_lag_ms.begin(), m_enh_lag_ms.end());
        }
        std::sort(sorted.begin(), sorted.end());
        int p50 = (int)sorted[sorted.size() / 2];
        int g = std::max(0, std::min(300, p50 + 30));
        int cur = m_grace_cur.load();
        if (std::abs(g - cur) > 20) {  // 变化 >20ms 才调整（防抖）
            m_grace_cur = g;
            m_scheduler.set_grace_ms(g);
            CLOG("[grace] enh lag p50=%dms -> grace=%dms (n=%zu)", p50, g, sorted.size());
        }
    }
}

void Client::store_residual(std::uint64_t pts_ms, std::vector<std::uint8_t> y,
                            std::vector<std::uint8_t> uv) {
    Residual r;
    r.y = std::move(y);
    r.uv = std::move(uv);
    std::lock_guard<std::mutex> lk(m_resid_mtx);
    while (m_resid.size() >= 4) m_resid.erase(m_resid.begin());  // 挤出最旧
    m_resid[pts_ms] = std::move(r);
}

void Client::clear_residuals(const char* why) {
    std::lock_guard<std::mutex> lk(m_resid_mtx);
    if (!m_resid.empty()) {
        CLOG("[resid] clear %zu frames (%s)", m_resid.size(), why);
        m_resid.clear();
    }
}

void Client::enh_loop() {
    H264Decoder enh_dec;   // kind=0x02 专用（与基础层解码器独立实例）
    std::vector<std::uint8_t> sym;
    while (m_running.load()) {
        tight::Bytes msg;
        {
            std::unique_lock<std::mutex> lk(m_enh_mtx);
            m_enh_cv.wait(lk, [&] { return !m_enh_queue.empty() || !m_running.load(); });
            if (!m_running.load()) break;
            msg = std::move(m_enh_queue.front());
            m_enh_queue.pop_front();
        }
        // [0x57][pts_ms u64be][kind u8][载荷]
        if (msg.size() < 10) continue;
        std::uint64_t pts_ms = 0;
        for (int i = 0; i < 8; ++i)
            pts_ms = (pts_ms << 8) | msg[1 + i];
        // 迟到残差：对应基础帧已上屏（或更旧）→ 直接丢弃
        if (pts_ms <= m_last_display_pts.load()) continue;
        std::uint8_t kind = msg[9];

        if (kind == 0x01) {
            // Rice 对照路径：熵解码 → sym 平面（planar）→ 交织 UV
            // （ver 字节与 kind 字节同位复用：decode 从 +9 起，含 ver=0x01）
            int w = m_enc_w.load();
            int h = m_enc_h.load();
            if (w <= 0 || h <= 0 || (w & 1) || (h & 1)) continue;
            sym.resize(layered::frame_bytes(w, h));
            if (!layered::decode_enhancement(msg.data() + 9, msg.size() - 9, w, h,
                                             sym.data())) {
                continue;
            }
            std::vector<std::uint8_t> ry(sym.begin(), sym.begin() + (std::size_t)w * h);
            std::vector<std::uint8_t> ruv((std::size_t)w * (h / 2));
            layered::interleave_uv(sym.data(), w, h, ruv.data());
            store_residual(pts_ms, std::move(ry), std::move(ruv));
        } else if (kind == 0x02) {
            // H.264 迷彩帧：[enh_flags u8(bit0=IDR)][Annex-B AU（IDR 含 SPS/PPS）]
            if (msg.size() < 12) continue;
            bool idr = (msg[10] & 0x01) != 0;
            if (!m_enh_got_idr.load() && !idr) {  // 断链/未初始化：等增强 IDR
                // 有增强帧在到但没见过 IDR（首 IDR 可能在建连前发出/丢失）→ 主动请求
                request_enh_keyframe();
                continue;
            }
            if (pts_ms <= m_last_enh_pts.load()) {        // 乱序 → 断链处理
                if (m_enh_got_idr.exchange(false)) CLOG("[enh-gate] reorder -> CLOSED");
                clear_residuals("enh-reorder");
                request_enh_keyframe();
                continue;
            }
            if (m_java_dec_ready.load()) {
                // Java MediaCodec 路径：解码输出经 on_java_decoded(layer=1) 入残差 map
                if (!m_java_dec[1].feed(msg.data() + 11, msg.size() - 11, pts_ms,
                                        idr ? 1 : 0)) {
                    if (!idr && m_enh_got_idr.exchange(false)) {
                        CLOG("[enh-gate] java-dec-busy -> CLOSED");
                        clear_residuals("enh-java-busy");
                        request_enh_keyframe();
                    }
                    continue;
                }
                if (idr && !m_enh_got_idr.exchange(true)) CLOG("[enh-gate] idr -> open");
                continue;
            }
            VideoFrame f;
            if (!enh_dec.decode(msg.data() + 11, msg.size() - 11, idr, f)) {
                // 解码失败（积压/异常）：P 帧漏解 = 断链 → 门控 + 请求增强 IDR
                if (!idr && m_enh_got_idr.exchange(false)) {
                    CLOG("[enh-gate] decode-fail -> CLOSED");
                    clear_residuals("enh-decode-fail");
                    request_enh_keyframe();
                }
                continue;
            }
            m_last_enh_pts.store(pts_ms);
            if (idr && !m_enh_got_idr.exchange(true)) CLOG("[enh-gate] idr -> open");
            if (f.width <= 0 || f.y.empty() || f.uv.empty()) continue;
            // 解码平面即 sym（128=零残差）：Y 直用，UV 已 NV12 交错
            store_residual(pts_ms, std::move(f.y), std::move(f.uv));
        }
    }
    enh_dec.shutdown();
}

// ---- 输入 / 音频 / 模式 ----

void Client::send_touch(std::uint8_t action, float x, float y) {
    tight::Bytes v;
    v.reserve(11);
    v.push_back(0x01);
    v.push_back(action);
    v.push_back(0);  // slot
    put_f32be(v, x);
    put_f32be(v, y);
    if (m_transport) m_transport->send_command(std::move(v));
}

void Client::send_key(std::uint8_t action, std::int32_t keycode) {
    tight::Bytes v;
    v.reserve(6);
    v.push_back(0x02);
    v.push_back(action);
    put_u32be(v, (std::uint32_t)keycode);
    if (m_transport) m_transport->send_command(std::move(v));
}

void Client::send_text(const std::string& utf8) {
    tight::Bytes v;
    v.reserve(1 + utf8.size());
    v.push_back(0x03);
    v.insert(v.end(), utf8.begin(), utf8.end());
    if (m_transport) m_transport->send_command(std::move(v));
}

void Client::send_scroll(float x, float y, float dy) {
    tight::Bytes v;
    v.reserve(13);
    v.push_back(0x04);
    put_f32be(v, x);
    put_f32be(v, y);
    put_f32be(v, dy);
    if (m_transport) m_transport->send_command(std::move(v));
}

void Client::send_set_format(std::uint8_t mode) {
    if (m_transport) m_transport->send_command({0x06, mode});
}

void Client::send_audio(const std::uint8_t* data, std::size_t size) {
    if (m_transport) m_transport->send_audio(data, size);
}
