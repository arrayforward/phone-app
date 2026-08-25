#include "transport_glue.h"

#include "commands.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#include <windows.h>  // GetCurrentProcessId

using namespace std::chrono;

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
    // 每次运行唯一 id：server 以 id 路由消息，重连用同名 id 会让消息
    // 发往尚未超时（dead_timeout）的旧会话地址，导致视频/数据丢失
    cfg.id    = "pc-" + std::to_string(GetCurrentProcessId());
    cfg.token = m_opts.token;
    cfg.role  = tight::LinkRole::Leaf;
    // 固定本地端口：手机侧（MIUI/Android 13 内核）观察到 server 的 UDP socket
    // 会被内核"connect"到首个对端地址，之后只接收来自该地址:端口的数据报
    // （其他来源被 NoPorts 丢弃）。client 固定端口后，重连/重启来源地址不变，
    // 始终能通过该过滤。
    cfg.bind  = tight::NetAddress("0.0.0.0", 18800);
    cfg.report_interval     = milliseconds(333);
    cfg.late_buffer_ms      = 16;
    cfg.max_message_bytes   = 1024 * 1024;  // 1MB，容纳大 IDR 帧
    cfg.retransmit_enabled  = true;         // ARQ 机制总开关（keep_pending 依赖对端握手通告；
                                            // 关闭它会让 channel_reliable 静默失效）
    // 视频通道（0）不开 ARQ：重传乱序送达会断 H.264 解码顺序 → 花屏。
    // 纯 FEC + 丢帧 REQ_KEYFRAME 恢复。
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
    // 丢帧（FEC 无法恢复）→ 立即请求关键帧 + 通知应用门控解码
    m_transport->set_message_loss_callback(
        [this](const std::string&, std::uint8_t channel) {
            if (channel != 0) return;
            request_keyframe();
            if (m_on_video_loss) m_on_video_loss();
        });
    m_transport->set_peer_callback(
        [this](const tight::PeerEvent& ev) {
            if (ev.id != kPeerId) return;
            if (ev.state == tight::LinkState::Online) {
                bool was = m_online.exchange(true);
                if (!was && m_on_state) m_on_state(true);
                // 建连后立即请求一次关键帧（protocol 第 3/7 节）
                request_keyframe();
            } else if (ev.state == tight::LinkState::Closed) {
                bool was = m_online.exchange(false);
                if (was && m_on_state) m_on_state(false);
            }
        });

    if (!m_transport->start()) {
        std::fprintf(stderr, "[transport] TightTransport::start() failed\n");
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
    // 掉线自动重连：未 Online 时每 2s 重试 connect（握手由 tight 异步完成，
    // 已连接时 connect 重复调用无害，这里用 online 标志门控）。
    while (m_running) {
        if (!m_online) {
            if (!m_transport->connect({kPeerId, {m_opts.host, m_opts.port}})) {
                std::fprintf(stderr, "[transport] connect(%s:%u) rejected\n",
                             m_opts.host.c_str(), (unsigned)m_opts.port);
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
    send_command(tc::build_req_keyframe());
}
