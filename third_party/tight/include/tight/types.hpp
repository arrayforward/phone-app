#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace tight {

using Bytes = std::vector<std::uint8_t>;

enum class PacketType : std::uint8_t {
    Handshake    = 0,
    HandshakeAck = 1,
    Online       = 2,
    Heartbeat    = 3,
    Bye          = 4,
    Data         = 5,
    Parity       = 6,
    Ack          = 7,
    Report       = 8,
    Probe        = 9,
    Command      = 10,
};

struct PacketHeader {
    std::uint32_t magic{};
    std::uint8_t  version{};
    PacketType    type{};
    std::uint16_t flags{};
    std::uint32_t client_id{};
    std::uint64_t session_id{};
    std::uint32_t sequence{};
    std::uint32_t acknowledgment{};
    std::uint32_t message_id{};
    std::uint16_t fragment_index{};
    std::uint16_t fragment_count{};
    std::uint16_t payload_size{};
    std::uint16_t reserved{};
    std::uint32_t tick{};
    std::uint32_t checksum{};
};

enum class LinkRole : std::uint8_t {
    Leaf = 0,
    Node = 1,
};

// lite 精简模式的业务画像：决定队列/缓冲收紧程度。
//   Audio：极致低内存——encode≤32、outbound≤64、socket≤8KB（音频 20ms 2 包
//     + 333ms 报告，64 条 ≈ 2s 余量）
//   Video：标准 lite 队列（encode≤64、outbound≤256），关闭 FEC（丢帧由
//     播放端 req-keyframe 兜底；接收不再被 RS 解码拖慢——lite 视频可行）
enum class LiteProfile : std::uint8_t {
    Audio = 0,
    Video = 1,
};

enum class LinkState : std::uint8_t {
    Closed      = 0,
    Handshake   = 1,
    Established = 2,
    Online      = 3,
};

struct PeerEvent {
    std::string  id;
    LinkRole     role{LinkRole::Leaf};
    LinkState    state{LinkState::Closed};
    std::uint32_t client_id{};
};

struct NetAddress {
    std::string  host;
    std::uint16_t port{};

    NetAddress() = default;
    NetAddress(std::string h, std::uint16_t p)
        : host(std::move(h)), port(p) {}
};

struct RemotePeer {
    std::string id;
    NetAddress address;
};

struct TightConfig {
    NetAddress    bind;
    std::string   id;
    std::string   token;
    LinkRole      role{LinkRole::Leaf};
    // 1350：单包载荷 1350-48(头)-16(GCM)=1286B，恰好整包容纳
    // 16kHz 单声道 PCM 40ms 帧（1280B），音视频场景免分片
    std::size_t    mtu{1350};
    std::chrono::milliseconds heartbeat{std::chrono::seconds(5)};
    std::chrono::milliseconds report_interval{std::chrono::seconds(1)};
    // 排空节拍。lite 模式自动钳制到 ≥10ms（IoT 设备降 CPU 唤醒/功耗，
    // 附加延迟 ≤10ms）；普通模式按配置值（可低至 1-2ms 保音频延迟）
    std::chrono::milliseconds flush_interval{std::chrono::milliseconds(10)};
    std::chrono::milliseconds dead_timeout{std::chrono::seconds(30)};
    std::chrono::milliseconds retransmit_timeout{std::chrono::milliseconds(500)};
    // 初始带宽估计（bytes/s）与 btl 提升上限（种子）：AIMD 从该值起步，
    // 恢复台阶提升不超过它。默认 10Mbps（=1250000 B/s）：实时音视频常见
    // 上限，避免从 100MB 大种子起步造成弱网段长时间超发。
    std::uint64_t  initial_bandwidth_bytes{3750000};  // 30Mbps：btl 种子与提升上限（弱网下由报告收敛）
    std::size_t    queue_limit{65536};
    // 单条应用消息的最大长度（默认 64KB）。发送超限返回 false；
    // 接收侧按此上限防御异常 fragment_count（防内存耗尽）。
    // 有效范围自动钳制到 [8KB, 10MB]。
    std::size_t    max_message_bytes{64 * 1024};
    // 丢弃异常消息（超限/畸形分片）时输出告警日志。仅服务器端开启；
    // lite_mode 端点自动关闭（静默丢弃），不受此开关影响。
    bool           drop_log{true};
    // 数据面 NACK 重传开关。关闭后：本端不生成 NACK、缺口立即跳过；
    // 并经握手能力标志通告对端——对端收到后不再为本链路保留重传缓冲
    // （m_pending≈0，在途内存显著下降），纯 FEC 兜底。任一端可单方面
    // 关闭，不影响业务：控制包（握手/命令）可靠性独立，数据面实时流
    // 由 FEC + 应用层容错覆盖。适用：lite IoT 实时音视频/遥测；
    // 文件/关键数据勿关。
    bool           retransmit_enabled{true};
    // A data packet is "late" when its transit time (one-way, computed with
    // the per-peer clock offset) exceeds late_rtt_multiplier * RTT.
    double           late_rtt_multiplier{4.0};
    // 迟到 buffer（毫秒）：由上层业务指定（视频场景 = 16ms，对应 33ms 帧
    // 周期的一半）。接收端统计单程传输时间直方图，迟到线 = P50 + late_buffer_ms。
    // 延迟超过迟到线的报文记为迟到（视频语义=丢，需 FEC 补），超线比例 p
    // 上报发送端驱动分段 FEC。0 = 未启用（回退 late_rtt_multiplier×RTT 判定）。
    std::uint32_t    late_buffer_ms{0};
    // 音频通道编码码率（bps）：tight 计算视频可用码率（video_capacity_bps）
    // 时先扣除此值；音频校验片开销按 channel_fec_extra[1] 的设置自动叠加
    // （预留 = audio_reserved_bps × (1 + channel_fec_extra[1])）——不设置
    // （0）即不预留校验，默认 0。应用按实际音频编码配置填入（如 128000）。
    std::uint32_t    audio_reserved_bps{0};
    // 令牌贷款时间窗（秒）：共享令牌桶（视频+file/data）允许视频透支
    // 的额度 = btl × loan_seconds。贷款用于：① 帧随心跳一次性发完（不
    // 被 10ms 打平）② 覆盖编码联动延迟（1-2s）——上层改码率失败/未生效
    // 期间允许有限超发。超限（连续弱网 + 联动失效）→ 清空视频队列并
    // 触发回调（应用重启编码器出关键帧 + 低码率），债务清零（token≥0）
    // 后自动恢复。默认 5s：覆盖弱网段 btl 触底 → 台阶回升（1-3s）期间
    // 视频下限（QSV 1.5M）> btl 的超发窗口，避免 1s 额度下的频繁
    // 耗尽-恢复循环（实测 15 次/25s → 视频断续 + 音频欠载）。0 = 禁用
    // 贷款（视频严格令牌）。
    double           loan_seconds{5.0};
    // 拥塞排空窗口（ms）：btl 量化大降（剧烈档 strength≥5%，×0.45 及
    // 以下）后，按触发时刻快照计算超发积压量 Q（梯形面积 = (R_old−R_new)
    // ×报告周期÷2）与排空码率，窗口内 video_capacity 回调/轮询输出
    // cap = btl_snap − Q/窗口（3s 内排完积压）→ 应用编码码率骤降 → 发送
    // 骤减 → 积压排空 → CE 早停（排空期 btl 连降轮数少、不崩底）。窗口
    // 结束自动恢复（btl 回升 → 码率回归跟随）。默认 3s = 人类可容忍的
    // 等待时长。0 = 禁用（回退无排空窗口的量化降速）。
    std::uint32_t    slowdown_window_ms{3000};
    // 每逻辑通道的额外 FEC 校验片数（索引 = 通道号）。通道 0 为默认（视频/
    // 数据），通道 1 常作为音频通道。额外校验片叠加在按 late_ratio 自适应的
    // 校验片之上：音频帧小且关键，可单独提高冗余（如通道 1 设 1~2）。
    // 通道号同时写入数据报 reserved 字段高 4 位，接收端据此识别所属通道。
    std::uint16_t    channel_fec_extra[8]{};
    // 每通道 FEC 开关（发送侧）：false = 该通道一律不生成校验片（起始保护/
    // 自适应/通道额外全部归零）。适用"丢了就丢"的增强层类流量（分层视频的
    // 残差层：丢包即弃不上屏，省校验片带宽）。接收侧无需配置（无校验片时
    // 缺口自然走丢失通知）。默认全开。
    bool             channel_fec_enabled[8] = {true, true, true, true,
                                               true, true, true, true};
    // per-channel 可靠开关（NACK/ARQ 重传）：置位的通道消息参与按缺口
    // 重传（发送端保留 m_pending、接收端对缺口生成 NACK），未置位通道
    // （如实时视频，纯 FEC 兜底低延迟）缺口立即跳过不重传。两端需一致
    // 配置。与全局 retransmit_enabled 独立：可靠通道不受其关闭影响。
    bool             channel_reliable[8]{};
    // After a link comes Online each end sends a speed-test train of blank
    // Probe datagrams; the receiver estimates the inbound bandwidth from the
    // packet arrival times and reports it back. Disable to skip probing.
    bool             speed_test_enabled{true};
    std::size_t      speed_test_bytes{100 * 1024};
    // ECDH + AEAD：握手阶段交换 X25519 公钥并 HKDF 派生会话密钥，
    // 数据分组（Data/Parity/Command）使用 AES-256-GCM 加密
    bool             encryption_enabled{true};
    // 资源占用：socket 内核缓冲（SO_RCVBUF/SO_SNDBUF 各自大小）与内部队列容量
    std::size_t    socket_buffer_bytes{8 * 1024 * 1024};
    std::size_t    encode_queue_limit{4096};
    std::size_t    outbound_queue_limit{65536};
    // 客户端精简模式：单线程（receiver/encode/sender 职责全部由 reactor
    // 节拍合并）、线程使用 64KB 小栈、内核缓冲与队列上限自动收紧
    // （socket_buffer≤16KB、encode≤64、outbound≤256、queue_limit≤128），
    // 面向嵌入式/单连接客户端；可经 set_lite_mode() 运行时动态切换。
    // lite 下按 lite_profile 进一步收紧（Audio 更小）并默认关闭 FEC
    // （fec_enabled 由 profile 决定：Audio/Video 均关——低内存低 CPU）。
    bool             lite_mode{false};
    LiteProfile      lite_profile{LiteProfile::Audio};
    // 数据面 FEC（分段冗余）总开关：false = 发送不生成校验片、接收不解码
    // （节省校验片在途缓冲 + RS 解码临时分配 + 解码 CPU——lite 模式目标；
    // 丢包由应用层容错兜底：音频 PLC、视频 req-keyframe）。非 lite 默认
    // true（现状：自适应 FEC 覆盖弱网）。
    bool             fec_enabled{true};
};

inline std::uint64_t unix_millis() {
    using namespace std::chrono;
    auto now = system_clock::now();
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(now.time_since_epoch()).count());
}

} // namespace tight
