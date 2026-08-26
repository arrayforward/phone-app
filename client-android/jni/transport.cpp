#include "transport.h"

#include <android/log.h>
#include <unistd.h>

#include <chrono>

#define TAG "tightcast-client"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

using namespace std::chrono;

namespace {

// 控制命令构造（协议 §5，全部大端）——与 Windows client/src/commands.h 同构
void put_u32be(tight::Bytes& v, std::uint32_t x) {
    v.push_back((std::uint8_t)(x >> 24));
    v.push_back((std::uint8_t)(x >> 16));
    v.push_back((std::uint8_t)(x >> 8));
    v.push_back((std::uint8_t)x);
}

}  // namespace

namespace tcmd {

inline tight::Bytes build_req_keyframe() { return {0x05}; }

}  // namespace tcmd

TransportGlue::TransportGlue(Options opts, VideoCallback on_video,
                             DataCallback on_data, StateCallback on_state)
    : m_opts(std::move(opts))
    , m_on_video(std::move(on_video))
    , m_on_data(std::move(on_data))
    , m_on_state(std::move(on_state)) {}

TransportGlue::~TransportGlue() { stop(); }

bool TransportGlue::start() {
    // TightConfig：严格按 docs/protocol.md 第 1 节
    tight::TightConfig cfg;
    cfg.id    = "android-" + std::to_string(getpid());  // 每次运行唯一，防僵尸会话串话
    cfg.token = m_opts.token;
    cfg.role  = tight::LinkRole::Leaf;
    // 固定本地端口：手机内核会把 server UDP socket "connect" 到首个对端，
    // 固定端口保证重连来源不变（protocol §1）
    cfg.bind  = tight::NetAddress("0.0.0.0", 18800);
    cfg.report_interval     = milliseconds(333);
    cfg.late_buffer_ms      = 16;
    cfg.max_message_bytes   = 1024 * 1024;
    cfg.retransmit_enabled  = true;
    // 视频通道（0）与增强通道（4）均不开 ARQ（默认 false）：重传乱序送达会断
    // H.264 解码顺序 → 花屏；增强层丢失本就按丢帧处理
    cfg.channel_reliable[3] = true;         // data 通道（DEVICE_INFO）可靠
    cfg.channel_fec_extra[1]= 1;            // 音频通道额外冗余
    cfg.flush_interval      = milliseconds(10);

    m_transport = std::make_unique<tight::TightTransport>(cfg);

    m_transport->set_message_callback(
        [this](const std::string&, tight::Bytes payload) {
            if (m_on_video) m_on_video(std::move(payload));
        });
    m_transport->set_data_callback(
        [this](const std::string&, tight::Bytes payload) {
            if (m_on_data) m_on_data(std::move(payload));
        });
    // 丢帧（FEC 无法恢复）→ 仅 ch 0 基础层请求关键帧 + 门控；ch 4 增强层
    // 断链走自己的恢复：门控等增强 IDR + 0x07 REQ_ENH_KEYFRAME
    m_transport->set_message_loss_callback(
        [this](const std::string&, std::uint8_t channel) {
            if (channel == 0) {
                request_keyframe();
                if (m_on_video_loss) m_on_video_loss();
            } else if (channel == 4) {
                if (m_on_enh_loss) m_on_enh_loss();
            }
        });
    m_transport->set_peer_callback(
        [this](const tight::PeerEvent& ev) {
            if (ev.id != kPeerId) return;
            if (ev.state == tight::LinkState::Online) {
                bool was = m_online.exchange(true);
                if (!was && m_on_state) m_on_state(true);
                request_keyframe();
            } else if (ev.state == tight::LinkState::Closed) {
                bool was = m_online.exchange(false);
                if (was && m_on_state) m_on_state(false);
            }
        });

    if (!m_transport->start()) {
        LOGE("TightTransport::start() failed");
        return false;
    }
    m_running = true;
    m_reconnect_thread = std::thread(&TransportGlue::reconnect_loop, this);
    return true;
}

void TransportGlue::stop() {
    m_running = false;
    if (m_reconnect_thread.joinable()) m_reconnect_thread.join();
    if (m_transport) m_transport->stop();
    m_transport.reset();
    m_online = false;
}

void TransportGlue::reconnect_loop() {
    // 掉线自动重连：未 Online 时每 2s 重试 connect
    while (m_running) {
        if (!m_online) {
            if (!m_transport->connect({kPeerId, {m_opts.host, m_opts.port}})) {
                LOGE("connect(%s:%u) rejected", m_opts.host.c_str(), (unsigned)m_opts.port);
            }
            for (int i = 0; i < 20 && m_running && !m_online; ++i)
                std::this_thread::sleep_for(milliseconds(100));
        } else {
            std::this_thread::sleep_for(milliseconds(200));
        }
    }
}

bool TransportGlue::send_command(tight::Bytes payload) {
    if (!m_transport || !m_online) return false;
    return m_transport->send_command(kPeerId, std::move(payload));
}

bool TransportGlue::send_audio(const std::uint8_t* data, std::size_t size) {
    if (!m_transport || !m_online) return false;
    // tag 0xA1 前缀：避开 tight 内部保留首字节 0x01/0x02/0x03（protocol §4）
    tight::Bytes payload(size + 1);
    payload[0] = 0xA1;
    std::memcpy(payload.data() + 1, data, size);
    return m_transport->send_channel(kPeerId, std::move(payload), 1);
}

void TransportGlue::request_keyframe() {
    send_command(tcmd::build_req_keyframe());
}
