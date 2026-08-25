#pragma once
// tight 传输封装：按 docs/protocol.md 第 1 节配置 Leaf 客户端，
// 负责 connect、掉线自动重连（每 2s 重试）、Online/丢帧时发 REQ_KEYFRAME。

#include "tight/tight.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

class TransportGlue {
public:
    struct Options {
        std::string   host;
        std::uint16_t port  = 8800;
        std::string   token = "tightcast";
    };
    // 视频帧消息（通道 0 语义，tight message 回调）
    using VideoCallback = std::function<void(tight::Bytes)>;
    // 可靠 data 消息（通道 3，DEVICE_INFO）
    using DataCallback  = std::function<void(tight::Bytes)>;
    // 连接状态变化：true=Online false=Closed
    using StateCallback = std::function<void(bool online)>;

    TransportGlue(Options opts, VideoCallback on_video,
                  DataCallback on_data, StateCallback on_state);
    ~TransportGlue();

    bool start();
    void stop();

    bool online() const { return m_online.load(); }
    // 对端上报的单程延迟中位数 P50（ms，无则 0）——网络传输时延指标
    std::uint32_t peer_p50_ms() const {
        return m_transport ? m_transport->peer_p50_ms(kPeerId) : 0;
    }
    // 视频消息丢失回调（tight 重组失败）：内部已发 REQ_KEYFRAME，
    // 此回调供应用层做解码门控（P 帧参考链断裂 → 等新 IDR 再送解码）
    void set_video_loss_callback(std::function<void()> cb) { m_on_video_loss = std::move(cb); }
    // 控制命令（command 通道，tight 线程安全，可在 GUI 线程直接调用）
    bool send_command(tight::Bytes payload);
    // 音频 PCM（通道 1）
    bool send_audio(const std::uint8_t* data, std::size_t size);
    void request_keyframe();

    static constexpr const char* kPeerId = "phone";

private:
    void reconnect_loop();

    Options       m_opts;
    VideoCallback m_on_video;
    DataCallback  m_on_data;
    StateCallback m_on_state;

    std::unique_ptr<tight::TightTransport> m_transport;
    std::thread        m_reconnect_thread;
    std::atomic<bool>  m_running{false};
    std::atomic<bool>  m_online{false};
    std::function<void()> m_on_video_loss;
};
