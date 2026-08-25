#pragma once

#include "tight/types.hpp"
#include "tight/socket.hpp"
#include "tight/packet_codec.hpp"
#include "tight/fec.hpp"
#include "tight/bandwidth.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tight {

class TightTransport {
public:
    using MessageCallback = std::function<void(const std::string& peer_id, Bytes payload)>;
    using PeerCallback    = std::function<void(const PeerEvent& event)>;
    using CommandCallback = std::function<void(const std::string& peer_id, Bytes payload)>;
    // 接收端消息重组失败回调：某条消息的分片经 FEC 仍无法恢复而丢失时
    // 调用（视频场景 = 一帧丢失，应用层可据此请求关键帧快速恢复画面）。
    // channel 为逻辑通道号（0=视频/数据，1=音频...），区分哪条流。
    // 回调在接收线程内同步执行，须快速返回（只做标记/发命令，勿阻塞）。
    using MessageLossCallback = std::function<void(const std::string& peer_id,
                                                   std::uint8_t channel)>;
    // 文件完整接收回调（file 通道）：重组完成后交付文件名与内容。
    using FileCallback = std::function<void(const std::string& peer_id,
                                            const std::string& name, Bytes data)>;
    // 可靠数据消息回调（data 通道）：消息级去重，每条只投递一次。
    using DataCallback = std::function<void(const std::string& peer_id, Bytes data)>;
    // 视频可用码率回调（bps）：tight 按有效带宽 − 音频固定预留（配置
    // audio_reserved_bps）− file/data 实时速率，再按实际 FEC 冗余折算后的
    // 编码码率。仅当变化超过迟滞（相对 >10% 且绝对 >100k）才回调，由
    // tight 内部专用通知线程调用（回调须快速返回，只做存储/编码器调整）。
    // 应用据此直接设置编码码率，无需自行折让。
    using VideoCapacityCallback = std::function<void(std::uint64_t bps)>;
    // 令牌贷款耗尽/恢复通知：exhausted=true 表示共享令牌桶（视频+file/data）
    // 贷款超限（btl×loan_seconds，默认 1s）——视频发送被 tight 暂停（持续
    // 排空视频通道至债务清零），应用应停止推送/降码率；exhausted=false 表示
    // 债务清零、发送恢复——应用应重启编码器（新 IDR 关键帧 + 低码率）快速
    // 恢复画面。回调由 tight 内部 sender 线程调用，须快速返回（只置标志）。
    using LoanExhaustedCallback = std::function<void(bool exhausted)>;
    // 拥塞排空窗口触发通知（量化大降 → tight 已清空视频积压并排空视频
    // 通道）：应用应重启编码器（force_keyframe → 新 IDR 关键帧 + 低码率），
    // 播放端据此跳到最新时间线，追赶/超发积压瞬间归零。回调由 tight
    // 内部接收/轮询线程调用，须快速返回（只置标志）。
    using EvacKeyframeCallback = std::function<void()>;

    explicit TightTransport(TightConfig config);
    // 外部 socket 模式（见 socket.hpp）：socket 非空时 tight 不创建/绑定/
    // 关闭任何 socket，也不启动接收线程——发送经 socket->send_to() 输出，
    // 接收由应用调用 inject_packet() 注入。socket 的生命周期归应用，
    // 须存活至本实例 stop()/析构之后。socket 为空等价于默认构造。
    TightTransport(TightConfig config, std::shared_ptr<ISocket> socket);
    ~TightTransport();

    TightTransport(const TightTransport&) = delete;
    TightTransport& operator=(const TightTransport&) = delete;

    void set_message_callback(MessageCallback callback);
    void set_peer_callback(PeerCallback callback);
    void set_command_callback(CommandCallback callback);
    void set_message_loss_callback(MessageLossCallback callback);
    void set_file_callback(FileCallback callback);
    void set_data_callback(DataCallback callback);
    // 视频可用码率通知（见 VideoCapacityCallback 注释）
    void set_video_capacity_callback(VideoCapacityCallback callback);
    // 令牌贷款耗尽/恢复通知（见 LoanExhaustedCallback 注释）
    void set_loan_exhausted_callback(LoanExhaustedCallback callback);
    // 拥塞排空窗口通知（见 EvacKeyframeCallback 注释）
    void set_evac_keyframe_callback(EvacKeyframeCallback callback);

    bool start();
    void stop();

    // 外部 socket 模式专用：注入一个从外部 socket 收到的完整数据报
    // （tight 报文，含 48B 头）。from 为对端的逻辑地址（须与该对端
    // connect() 时的地址一致；单连接通道可固定填对端地址）。在应用自己
    // 的收包线程内调用，tight 在该线程内同步完成解码与分发（应用回调
    // 可能在该线程内触发）。默认（内部 socket）模式下调用无效。
    void inject_packet(const NetAddress& from, const std::uint8_t* data, std::size_t size);
    void inject_packet(const NetAddress& from, const Bytes& datagram);

    bool connect(const RemotePeer& remote);
    bool send(const std::string& peer_id, Bytes payload);
    // 发送视频帧（视频通道 = 0，贷款连发）：keyframe 表明本帧是否为关键帧
    // （IDR）。tight 在拥塞排空窗口内据此识别"新 IDR 已提交发送"——窗口
    // 结束条件 = IDR 发出 + 下一报告到达，从而把追赶/超发积压一次清零。
    bool send_video(const std::string& peer_id, Bytes payload, bool keyframe);
    // 发送到指定逻辑通道（通道号 0..7，写入数据报 flags，接收端可识别）。
    // 各通道可经 TightConfig::channel_fec_extra 独立设置额外 FEC 冗余，
    // 例如通道 1 作音频通道单独加强冗余。
    bool send_channel(const std::string& peer_id, Bytes payload, std::uint8_t channel);
    bool send_priority(const std::string& peer_id, Bytes payload, int priority);

    // 发送文件（file 通道 = 2，可靠 ARQ）：先发文件清单（文件名+分块
    // 信息），再逐块发送，块级 NACK 重传保证完整到达。接收端完整重组
    // 后经 set_file_callback 交付。name 长度 ≤65535，data 任意大小。
    // 返回 false = 队列背压或通道未可靠配置。
    bool send_file(const std::string& peer_id, const std::string& name, const Bytes& data);
    // 发送可靠数据消息（data 通道 = 3，ARQ 重传 + 消息级去重 only once）。
    // 接收端经 set_data_callback 交付。
    bool send_data(const std::string& peer_id, Bytes payload);

    // Sends a command packet (control / button-key information). Commands
    // fit in a single datagram, so no fragmentation is needed, and they jump
    // the queue ahead of any pending data. The peer receives them via its
    // CommandCallback in order: out-of-order packets are held for at most
    // 3 RTT before the gap is skipped (late arrivals are then dropped).
    // Returns false when the payload exceeds one datagram.
    bool send_command(const std::string& peer_id, Bytes payload);

    // 运行时动态切换精简模式（本端本地属性，不影响对端）：
    //   true  —— 单线程（receiver/encode/sender 职责并入 reactor 节拍），
    //            64KB 小栈、小缓冲小队列，空闲实例约 76KB
    //   false —— 4 线程（reactor/receiver/encode/sender 全分离）
    // 队列容量按构造时配置固定，切换只改变线程模型；start() 前后均可调用。
    void set_lite_mode(bool lite);
    bool lite_mode() const;

    std::vector<PeerEvent> peers() const;
    std::uint16_t local_port() const;

    // 诊断接口：当前带宽估计（bytes/s，BtlBw×gain 的限速值）与
    // 发送状态。供应用观测拥塞控制行为/测试验证，不影响协议。
    std::uint64_t estimated_bandwidth_bps() const;
    std::uint64_t btl_bw_bps() const;
    // 视频可用码率（bps）：有效带宽 − 音频固定预留 − file/data 实时速率，
    // 按实际 FEC 冗余折算后的编码码率（= video_capacity_bps，轮询版）。
    std::uint64_t video_capacity_bps() const;
    // 实际 FEC 冗余率（校验片/数据片，滑动窗口 1s，0~N）。
    double fec_redundancy_ratio() const;
    bool pacer_app_limited() const;
    bool pacer_limited() const;
    // 对端上报的单程延迟中位数 P50（ms，无该 peer 或未上报时返回 0）：
    // 应用可据此做延迟信号码率控制（P50 高 = 拥塞积压的直接证据）。
    std::uint32_t peer_p50_ms(const std::string& peer_id) const;

    // 出站积压数据报总数（send + encode + outbound 队列）：应用据此检测
    // 网络拥塞导致的发送队列阻塞（本地即时信号，先于对端报告）。
    std::size_t outbound_queue_size() const;
    // 清空出站积压（丢帧止损）：丢弃全部待发送数据报（仅数据面，握手/
    // 报告/命令保留）。视频场景：网络变差编码器降码率前队列已阻塞时调用，
    // 配合应用侧 force_keyframe 让链路快速恢复到最新画面。
    void clear_outbound();
    // 按通道排空（丢帧止损，不清队列）：排空期内指定通道的数据报在出队
    // 时直接丢弃（不占带宽、不发），其他通道（音频/文件/数据）完全不受
    // 影响。发送队列、编码队列、发送线程三层检查，确保排空期后的旧消息
    // 不会从管线漏出。应用在积压止损/带宽骤降时调用（配合编码器重启，
    // 让新 IDR 在排空期后正常发送），期满自动恢复。
    //   drain_channel(ch)          ：排空时长 = 内部默认（100ms）
    //   drain_channel(ch, duration)：显式指定排空时长
    void drain_channel(std::uint8_t channel);
    void drain_channel(std::uint8_t channel, std::chrono::milliseconds duration);
    // file/data 通道待发送负载（字节）：应用据此做带宽预算
    // （file+data 有负载时 video 让出一半 btl）。
    std::uint64_t file_data_pending_bytes() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace tight
