#pragma once
// tight Leaf 传输封装（Android 客户端）：移植自 Windows client/src/transport_glue.cpp，
// 平台差异：getpid() 替代 GetCurrentProcessId。
//
// 收到的全部数据消息（ch0 视频 tag 0x56 / ch4 增强 tag 0x57）经同一回调上抛，
// 由上层按 tag 分发。丢帧回调按 channel 过滤：仅 ch 0（基础层）丢失才请求关键帧。

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
    // 数据消息（ch0 视频 / ch4 增强，含 tag 前缀，payload 原样）
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
    std::uint32_t peer_p50_ms() const {
        return m_transport ? m_transport->peer_p50_ms(kPeerId) : 0;
    }
    // 视频消息丢失回调（tight 重组失败）：channel==0 时内部已发 REQ_KEYFRAME，
    // 供应用层做解码门控（P 帧参考链断裂 → 等新 IDR 再送解码）
    void set_video_loss_callback(std::function<void()> cb) { m_on_video_loss = std::move(cb); }
    // 增强层（ch 4）丢失回调：增强 H.264 流断链 → 应用层应门控等增强 IDR 并发 0x07
    void set_enh_loss_callback(std::function<void()> cb) { m_on_enh_loss = std::move(cb); }
    // 控制命令（command 通道，tight 线程安全）
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
    std::function<void()> m_on_enh_loss;
};
