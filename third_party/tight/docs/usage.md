# tight 协议使用文档

tight 是一个自包含、零第三方依赖的 C++17 可靠 UDP 传输库，面向端云实时通信
场景（IoT 设备 ↔ 云端网关）。本文档覆盖：集成方式、快速上手、完整的
客户端/服务器端示例、配置项参考、API 参考与行为约定。

## 目录

- [1. 特性一览](#1-特性一览)
- [2. 集成](#2-集成)
- [3. 快速上手（最小示例）](#3-快速上手最小示例)
- [4. 完整客户端示例（IoT 音频设备）](#4-完整客户端示例iot-音频设备)
- [5. 完整服务器端示例（多设备网关）](#5-完整服务器端示例多设备网关)
- [6. TightConfig 配置参考](#6-tightconfig-配置参考)
- [7. API 参考](#7-api-参考)
- [8. 行为约定（必读）](#8-行为约定必读)
- [9. 典型场景参数配方](#9-典型场景参数配方)
- [10. 外部 socket 模式（UDP/WebSocket/TCP 等自定义通道）](#10-外部-socket-模式udpwebsockettcp-等自定义通道)

---

## 1. 特性一览

| 特性 | 说明 |
|---|---|
| 可靠传输 | ACK 确认 + NACK 丢包重传（每包最多 10 次），缺口超过 3.5×RTT 即上报，确认前每个报告周期重复 NACK |
| 加密 | 握手 X25519 密钥交换，HKDF-SHA256 派生会话密钥，数据面 AES-256-GCM（报文头做 AAD），可关 |
| FEC | Reed-Solomon GF(2⁸) 擦除码，冗余率由迟到率信息熵 H(p)×1.2 动态驱动 |
| 拥塞控制 | 三信号 AIMD（GCC 风格）：排队延迟（P50−RTprop）EWMA>20ms 或帧级迟到率>2%（关键帧突刺不算）或丢包率>2%（令牌受限时）或 CE>1% 判拥塞；**突刺门控** + **量化降速阶梯**（按强度 ×0.20~×0.65，一次到位不崩底）；恢复两步台阶 ×1.5（FEC 探测先行）；下限 100kbps，提升上限 = 种子 |
| 时钟对表 | 握手对表 + 每次心跳重对表，可测单向传输时间（慢报文统计） |
| 命令通道 | 单报文控制指令，保序投递（乱序最多等 3×RTT），插队直达 |
| 优先级 | 数据消息支持优先级，高优先级先出队（音频不被文件流阻塞） |
| 精简模式 | 客户端单线程、64KB 小栈、小缓冲小队列，空闲实例 ~76KB，可运行时切换 |

## 2. 集成

```mermaid
flowchart LR
    subgraph Host["宿主工程"]
        APP["应用代码<br/>(视频/音频/文件/遥测)"]
        T["tight::TightTransport"]
    end
    subgraph Tight["tight 库"]
        API["include/tight/*"]
        SRC["src/*（私有实现）"]
    end
    subgraph OS["系统层"]
        SOCK["socket（ws2_32 / POSIX）"]
    end
    APP -->|"add_subdirectory(tight) +<br/>target_link_libraries(... tight)"| T
    T --> API --> SRC --> SOCK
```

### 方式一：add_subdirectory（推荐）

```cmake
add_subdirectory(tight)
target_link_libraries(your_app PRIVATE tight)
# Windows 需链接 ws2_32（tight 的 CMake 已处理）
```

### 方式二：独立构建安装

```bash
cmake -B build -S tight -G "MinGW Makefiles"
cmake --build build -j 4
cmake --install build --prefix /your/prefix
# 头文件: prefix/include/tight/  库: prefix/lib/libtight.a
```

代码中包含：

```cpp
#include "tight/tight.hpp"   // 聚合头：TightTransport + TightConfig + 全部类型
```

## 3. 快速上手（最小示例）

### 3.1 服务器（echo）

```cpp
#include "tight/tight.hpp"
#include <cstdio>

int main() {
    tight::TightConfig cfg;
    cfg.bind  = tight::NetAddress("0.0.0.0", 9443);
    cfg.id    = "gateway";
    cfg.token = "shared-secret";
    cfg.role  = tight::LinkRole::Node;      // 服务器必须是 Node

    tight::TightTransport server(cfg);

    server.set_message_callback(
        [&server](const std::string& peer, tight::Bytes payload) {
            std::printf("[msg] %s: %zu bytes\n", peer.c_str(), payload.size());
            server.send(peer, std::move(payload));   // echo 回去
        });
    server.set_peer_callback([](const tight::PeerEvent& e) {
        std::printf("[peer] %s state=%d\n", e.id.c_str(), static_cast<int>(e.state));
    });

    if (!server.start()) return 1;
    std::puts("listening on 9443...");
    std::getchar();
    server.stop();
}
```

### 3.2 客户端

```cpp
#include "tight/tight.hpp"
#include <cstdio>
#include <cstring>

int main() {
    tight::TightConfig cfg;
    cfg.bind      = tight::NetAddress("0.0.0.0", 0);   // 端口 0 = 系统分配
    cfg.id        = "device-001";
    cfg.token     = "shared-secret";
    cfg.lite_mode = true;                              // 客户端精简模式

    tight::TightTransport client(cfg);
    client.set_message_callback([](const std::string&, tight::Bytes payload) {
        std::printf("[echo] %.*s\n", (int)payload.size(), (const char*)payload.data());
    });

    if (!client.start()) return 1;
    if (!client.connect({"gateway", tight::NetAddress("127.0.0.1", 9443)})) return 1;

    const char* text = "hello tight";
    client.send("gateway", tight::Bytes(text, text + std::strlen(text)));

    std::getchar();
    client.stop();
}
```

## 4. 完整客户端示例（IoT 音频设备）

场景：16kHz 单声道 G.711 音频上行 + 视频流 + 偶尔的文件上传，
lite 模式，断线自动重连（协议内置），40ms 组包。

```cpp
#include "tight/tight.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace std::chrono;

class AudioDevice {
public:
    AudioDevice() {
        tight::TightConfig cfg;
        cfg.bind      = tight::NetAddress("0.0.0.0", 0);
        cfg.id        = "device-001";
        cfg.token     = "shared-secret";

        // --- IoT 场景配方（详见第 9 节）---
        cfg.lite_mode            = true;              // 单线程低占用
        cfg.mtu                  = 1350;              // 载荷 1286B，整包纳 PCM 40ms 帧
        cfg.max_message_bytes    = 64 * 1024;         // 文件块上限
        cfg.flush_interval       = milliseconds(10);  // lite 下自动钳制 ≥10ms，省 CPU
        cfg.report_interval      = milliseconds(500);
        cfg.speed_test_enabled   = true;              // 视频/文件需要准确带宽 pacing
        cfg.initial_bandwidth_bytes = 4 * 1024 * 1024;
        cfg.late_rtt_multiplier  = 2.5;               // FEC 更快响应劣化
        cfg.encryption_enabled   = true;

        m_client = std::make_unique<tight::TightTransport>(cfg);

        m_client->set_message_callback(
            [](const std::string&, tight::Bytes payload) {
                // 下行音频帧：送入解码播放
                std::printf("[downlink] %zu bytes\n", payload.size());
            });
        m_client->set_command_callback(
            [](const std::string&, tight::Bytes cmd) {
                // 云端控制指令（保序到达）：云台/按键/参数下发
                std::printf("[cmd] %.*s\n", (int)cmd.size(), (const char*)cmd.data());
            });
        m_client->set_peer_callback([this](const tight::PeerEvent& e) {
            m_online = (e.state == tight::LinkState::Online);
        });
    }

    bool run(const char* host, std::uint16_t port) {
        if (!m_client->start()) return false;
        if (!m_client->connect({"gateway", tight::NetAddress(host, port)})) return false;

        // 音频线程：40ms 一帧（G.711 640B）
        std::thread audio([this] {
            auto next = steady_clock::now();
            while (m_running) {
                next += milliseconds(40);
                std::this_thread::sleep_until(next);
                if (!m_online) continue;
                tight::Bytes frame = capture_g711_40ms();     // 采集+编码
                // 队列满（返回 false）直接丢帧：陈旧音频不如丢弃
                m_client->send_priority("gateway", std::move(frame), 2);
            }
        });

        // 视频线程：按码率 pacing，中优先级
        std::thread video([this] {
            while (m_running) {
                if (!m_online) { std::this_thread::sleep_for(milliseconds(100)); continue; }
                tight::Bytes chunk = capture_video_chunk();   // 8~16KB
                while (!m_client->send_priority("gateway", std::move(chunk), 1))
                    std::this_thread::sleep_for(milliseconds(5));
                pace_to_bitrate(512 * 1024 / 8, chunk.size());
            }
        });

        audio.join();
        video.join();
        m_client->stop();
        return true;
    }

private:
    tight::Bytes capture_g711_40ms() { return tight::Bytes(640, 0x55); }
    tight::Bytes capture_video_chunk() { return tight::Bytes(8192, 0x11); }
    void pace_to_bitrate(std::size_t, std::size_t) {
        std::this_thread::sleep_for(milliseconds(120));
    }

    std::unique_ptr<tight::TightTransport> m_client;
    std::atomic<bool> m_running{true};
    std::atomic<bool> m_online{false};
};

int main() {
    AudioDevice dev;
    return dev.run("127.0.0.1", 9443) ? 0 : 1;
}
```

要点：

- **优先级**：音频=2、视频=1、文件=0（`send_priority` 第三参数），
  文件 bulk 永远不会堵在音频前面
- **`send` 返回 false** 表示队列满或超 `max_message_bytes`：音频/视频丢帧，
  文件退避重试
- **断线**：协议内部自动重连（Node 端 `m_reconnect`），应用只需用
  `PeerEvent.state` 判断当前是否 Online

## 5. 完整服务器端示例（多设备网关）

```cpp
#include "tight/tight.hpp"

#include <cstdio>
#include <string>
#include <thread>
#include <unordered_map>

class Gateway {
public:
    Gateway() {
        tight::TightConfig cfg;
        cfg.bind  = tight::NetAddress("0.0.0.0", 9443);
        cfg.id    = "gateway";
        cfg.token = "shared-secret";
        cfg.role  = tight::LinkRole::Node;      // 必须 Node：接受设备接入

        // 服务器保持普通模式（4 线程），仅对齐业务参数
        cfg.mtu                  = 1350;
        cfg.max_message_bytes    = 64 * 1024;
        cfg.flush_interval       = std::chrono::milliseconds(2);  // 保音频链路
        cfg.queue_limit          = 4096;   // 多设备汇聚，给足缓冲深度
        cfg.drop_log             = true;   // 异常消息丢弃告警（服务器端开启）

        m_server = std::make_unique<tight::TightTransport>(cfg);

        m_server->set_peer_callback([](const tight::PeerEvent& e) {
            std::printf("[peer] %s state=%d client_id=%u\n",
                        e.id.c_str(), static_cast<int>(e.state), e.client_id);
        });

        m_server->set_message_callback(
            [this](const std::string& peer, tight::Bytes payload) {
                dispatch(peer, std::move(payload));
            });

        m_server->set_command_callback(
            [this](const std::string& peer, tight::Bytes cmd) {
                // 设备上行控制信息（按键/告警），保序到达
                std::printf("[cmd] %s: %.*s\n",
                            peer.c_str(), (int)cmd.size(), (const char*)cmd.data());
            });
    }

    bool run() {
        if (!m_server->start()) return false;
        std::printf("gateway on 9443, port=%u\n", m_server->local_port());
        std::getchar();
        m_server->stop();
        return true;
    }

private:
    void dispatch(const std::string& peer, tight::Bytes payload) {
        // 按业务首字节分流：0x01=音频 0x02=视频 0x03=文件块
        if (payload.empty()) return;
        switch (payload[0]) {
        case 0x01: forward_to_audio_pipeline(peer, payload); break;
        case 0x02: forward_to_video_pipeline(peer, payload); break;
        case 0x03: handle_file_chunk(peer, payload);         break;
        default: break;
        }
    }

    void forward_to_audio_pipeline(const std::string&, const tight::Bytes&) {}
    void forward_to_video_pipeline(const std::string&, const tight::Bytes&) {}

    void handle_file_chunk(const std::string& peer, const tight::Bytes& payload) {
        // 文件块：应用层序号校验，缺块经命令通道请求补发
        // （协议层重传上限 10 次，耗尽后静默丢弃，文件需应用层兜底）
        // send_command() 单报文保序，适合补发请求：
        //   m_server->send_command(peer, make_resend_request(missing_seq));
        (void)peer; (void)payload;
    }

    std::unique_ptr<tight::TightTransport> m_server;
};

int main() {
    Gateway gw;
    return gw.run() ? 0 : 1;
}
```

## 6. TightConfig 配置参考

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `bind` | NetAddress | — | 绑定地址；端口 0 = 系统分配（可用 `local_port()` 查询） |
| `id` | std::string | — | 本端标识，握手时交换；空 = 匿名（服务器分配 anon-*） |
| `token` | std::string | — | 接入令牌，握手校验 |
| `role` | LinkRole | Leaf | Node = 接受接入（服务器）；Leaf = 终端设备 |
| `mtu` | size_t | **1350** | 链路 MTU；单包载荷 = mtu-48（开 GCM 再 -16 = 1286B） |
| `heartbeat` | ms | 5000 | 心跳周期，兼作时钟重对表 |
| `report_interval` | ms | 1000 | ACK/NACK 报告周期（丢失序号确认前每周期重复上报） |
| `flush_interval` | ms | 10 | 排空节拍；**lite 模式自动钳制 ≥10ms**（IoT 省 CPU） |
| `dead_timeout` | ms | 30000 | 对端静默判定死亡 |
| `retransmit_timeout` | ms | 500 | 握手重传间隔 |
| `initial_bandwidth_bytes` | uint64 | **3.75MB（30Mbps）** | AIMD 初始 btl 与**提升上限**（种子）；弱网下由报告量化收敛 |
| `audio_reserved_bps` | uint32 | 0 | 音频编码码率，`video_capacity_bps` 计算时扣除（校验片按 `channel_fec_extra[1]` 叠加） |
| `loan_seconds` | double | 5.0 | 令牌贷款时间窗：视频可透支额度 = btl×loan_seconds（覆盖编码联动延迟 1~2s）；0 = 禁用（视频严格令牌） |
| `slowdown_window_ms` | uint32 | 3000 | 拥塞排空窗口：剧烈降速后窗口内 `video_capacity_bps` 输出排空码率（3s 排完超发积压，CE 早停不崩底）；0 = 禁用 |
| `queue_limit` | size_t | 65536 | 发送队列消息数上限（**lite ≤128**） |
| `max_message_bytes` | size_t | 64KB | 单消息上限，钳制 [8KB, 10MB]；超限 `send` 返回 false，接收侧丢弃畸形分片组 |
| `drop_log` | bool | true | 丢弃异常消息时告警（**lite 强制关闭**） |
| `retransmit_enabled` | bool | true | 数据面 NACK 重传。关闭：不生成 NACK、缺口立即跳过，并经握手通告对端停止保留重传缓冲；任一端可单方面关闭，纯 FEC 兜底 |
| `late_rtt_multiplier` | double | 4.0 | 慢报文判定阈值（倍 RTT）；调低 → FEC 响应更灵敏 |
| `speed_test_enabled` | bool | true | 建连后发探测列车测带宽 |
| `speed_test_bytes` | size_t | 100KB | 探测列车长度 |
| `encryption_enabled` | bool | true | ECDH + AES-256-GCM |
| `socket_buffer_bytes` | size_t | 8MB | SO_RCVBUF/SO_SNDBUF（**lite ≤16KB**） |
| `encode_queue_limit` | size_t | 4096 | 待分片消息队列（**lite ≤64**） |
| `outbound_queue_limit` | size_t | 65536 | 待发数据报队列（**lite ≤256**） |
| `lite_mode` | bool | false | 客户端精简模式；`set_lite_mode()` 可运行时切换 |
| `lite_profile` | LiteProfile | Audio | lite 业务画像：Audio = 极致低内存单线程（encode≤16/outbound≤32/queue≤64/socket≤4KB）；Video = 独立 receiver 双线程（encode≤64/outbound≤256/queue≤128/socket≤16KB） |
| `fec_enabled` | bool | true | 数据面 FEC 总开关：false = 不生成/不解码校验片（省在途缓冲 + RS 解码 CPU），丢包由应用层容错兜底（音频 PLC、视频 req-keyframe） |

## 7. API 参考

```cpp
class TightTransport {
public:
    using MessageCallback = std::function<void(const std::string& peer_id, Bytes payload)>;
    using PeerCallback    = std::function<void(const PeerEvent& event)>;
    using CommandCallback = std::function<void(const std::string& peer_id, Bytes payload)>;
    using MessageLossCallback = std::function<void(const std::string& peer_id, std::uint8_t channel)>;
    using FileCallback = std::function<void(const std::string& peer_id,
                                            const std::string& name, Bytes data)>;
    using DataCallback = std::function<void(const std::string& peer_id, Bytes data)>;
    using VideoCapacityCallback = std::function<void(std::uint64_t bps)>;  // 视频可用码率
    using LoanExhaustedCallback = std::function<void(bool exhausted)>;     // 令牌贷款耗尽/恢复
    using EvacKeyframeCallback = std::function<void()>;                    // 排空窗口触发（重启编码器出 IDR）

    explicit TightTransport(TightConfig config);

    void set_message_callback(MessageCallback);            // 数据消息（重组后投递）
    void set_peer_callback(PeerCallback);                  // 对端状态变化（Online/Closed...）
    void set_command_callback(CommandCallback);            // 命令通道（保序）
    void set_message_loss_callback(MessageLossCallback);   // 重组失败（丢帧通知，带通道号）
    void set_file_callback(FileCallback);                  // 文件完整接收
    void set_data_callback(DataCallback);                  // 可靠数据消息（去重）
    void set_video_capacity_callback(VideoCapacityCallback); // 视频可用码率通知（专用线程）
    void set_loan_exhausted_callback(LoanExhaustedCallback); // 令牌贷款耗尽/恢复（sender 线程）
    void set_evac_keyframe_callback(EvacKeyframeCallback);   // 排空窗口触发（接收/轮询线程）

    bool start();                                          // 绑定 + 起线程
    void stop();

    bool connect(const RemotePeer& remote);                // {id, NetAddress}
    bool send(const std::string& peer_id, Bytes payload);                  // 优先级 0
    bool send_channel(const std::string& peer_id, Bytes payload, std::uint8_t channel);
    bool send_priority(const std::string& peer_id, Bytes payload, int priority);
    bool send_file(const std::string& peer_id, const std::string& name, const Bytes& data);
    bool send_data(const std::string& peer_id, Bytes payload);
    bool send_command(const std::string& peer_id, Bytes payload);          // 单报文，超限 false
    bool send_video(const std::string& peer_id, Bytes payload, bool keyframe); // 视频帧（keyframe=IDR）

    void set_lite_mode(bool lite);                         // 运行时切换线程模型，本端属性
    bool lite_mode() const;

    std::vector<PeerEvent> peers() const;                  // 当前对端快照
    std::uint16_t local_port() const;

    // 诊断 / 码率 / 止损接口
    std::uint64_t estimated_bandwidth_bps() const;         // 当前限速 = btl
    std::uint64_t btl_bw_bps() const;                      // AIMD 估计 btl（bytes/s）
    std::uint64_t video_capacity_bps() const;              // 视频可用码率（bps，轮询版）
    double fec_redundancy_ratio() const;                   // 实际 FEC 冗余率（1s 窗口）
    bool pacer_app_limited() const;
    bool pacer_limited() const;                            // 本地令牌限速中（AIMD 否决拥塞）
    std::uint32_t peer_p50_ms(const std::string& peer_id) const;  // 对端单程延迟中位数
    std::size_t outbound_queue_size() const;               // 出站积压数据报数
    void clear_outbound();                                 // 清空数据面积压（保留音频）
    void drain_channel(std::uint8_t channel);              // 按通道排空 100ms（出队即丢）
    void drain_channel(std::uint8_t channel, std::chrono::milliseconds duration);
    std::uint64_t file_data_pending_bytes() const;         // file/data 待发负载
};
```

`PeerEvent`：`id` / `role` / `state`（Closed→Handshake→Established→Online）/ `client_id`。

辅助组件（可单独使用，见各自头文件）：

- `tight/fec.hpp`：`ReedSolomon::encode/decode`（span 接口，零拷贝）
- `tight/bandwidth.hpp`：三信号 AIMD 带宽估计器
- `tight/packet_codec.hpp`：线格式编解码 + CRC32
- `tight/blocking_queue.hpp`：有界阻塞队列（节点回收池）
- `tight/logger.hpp`：`TIGHT_LOG_*` 宏

## 8. 行为约定（必读）

1. **`send*` 返回 false 的三种情况**：未 start / 队列满 / 超过
   `max_message_bytes`。实时流丢帧，文件流退避重试。
2. **重传上限**：每包最多重传 10 次（NACK 驱动），耗尽后静默丢弃。
   实时音视频无感（FEC + 丢帧掩盖）；**文件传输必须在应用层做
   块序号校验 + 缺块补发 + 整文件校验**（补发请求走 `send_command`）。
3. **缺口跳过**：收端发现缺口超过 3.5×RTT 即跳过（ACK 游标不停滞），
   迟到的重传仍正常投递——乱序不会造成消息丢失。
4. **命令通道**：单报文（≤ 单包载荷）、保序、乱序最多等 3×RTT 后跳号。
5. **lite 模式是本端属性**：客户端 lite、服务器普通模式互不影响；
   握手/加密/测速等协议行为两端一致。
6. **内存**：空闲 lite 实例 ~76KB；传输期驻留 ≈ 在途字节数，与总流量
   无关（平台期，已验证 16MB 传输 delta ~136KB）。
7. **重传协商（握手能力标志）**：握手载荷尾部 1 字节 bit0 通告
   `retransmit_enabled`。发送端保留重传缓冲的条件 = 本端配置 && 对端
   通告，因此**任一端可单方面关闭整条链路的重传**（云端可按设备类型
   下发，端侧也可上报）。关闭后控制包（握手/命令）可靠性不受影响，
   数据面靠 FEC + 应用层容错。内存收益：在途增量从 ∝码率×确认窗口
   （50KB/s 实测 128KB，2Mbps 视频预估 ~500KB）降为**常数 ~24KB**；
   无重传协议栈预算可按 **76KB 静态 + ~50KB 动态** 封顶。
8. **拥塞控制（AIMD，统一柔表 + 迟滞 + 突刺门控 + 排空冻结）**：报告期
   平均发送 > 对端接收速率（持续超发）时按**统一柔表**降速（strength =
   max(late, loss, ce)，CE 与 late/loss 共享 ×0.90~×0.50 渐变收敛——急表曾
   致编码器频繁重启/设备崩溃）；关键帧突刺（瞬时超发）不降速；**排空窗口
   内 btl 完全冻结**；恢复需延迟 <10ms 且迟到率<0.5%（中间区保持防摆动），
   两步台阶 ×1.5（上限 = min(`initial_bandwidth_bytes` 种子默认 30Mbps,
   对端接收速率×1.2)）；下限 100kbps；令牌受限只信 CE；起步 probe 校准
   （btl 不超实测链路）。RTT>200ms 或 CE>1% 时 FEC 冗余自动关闭让出带宽；
   降幅 ≥35%（strength≥20%）进入 `slowdown_window_ms` 排空窗口 fast 模式
   （清队列 + `EvacKeyframeCallback` 出 IDR）；降幅 25%（×0.75）走 slow
   Q 面积法 3s 排空（不跳帧）。
9. **报告停滞降速**：Report 连续 3×`report_interval` 收不到（默认 1s×3=3s，
   视频 333ms×3≈1s）= 链路严重卡顿/断流 → btl ×0.5 **单次降**（one-shot，
   下限 100kbps），报告恢复后恢复台阶自动爬升。报告直发绕过令牌桶与
   队列（本地限速不会导致报告丢失），故停滞必为链路/对端问题；仅
   Online/Established 链路、全部活跃 peer 均停滞才触发，握手时重置报告
   时间戳——该机制**早于**贷款硬止损（~5s）与掉线检测（30s）。
10. **视频码率与止损**：`set_video_capacity_callback` 的回调在**专用通知
   线程**执行（须快速返回），码率变化 >10% 且 >100kbps 才触发；
   `drain_channel(ch)` 排空期内该通道数据报出队即丢（不清队列），音频/
   文件通道不受影响——积压止损后配合编码器重启，让新 IDR 在排空期外
   正常发送。
11. **令牌贷款（`loan_seconds`）**：视频（ch0）可透支 btl×贷款秒数（覆盖
    编码联动延迟）；`set_loan_exhausted_callback` 在 **sender 线程**回调
    （须快速返回）：`true` = 贷款耗尽、视频被持续排空（应用停止推流/降
    码率），`false` = 债务清零、发送恢复（应用重启编码器出关键帧 + 低码
    率）。音频通道（ch1）独立队列绕过令牌桶，不受贷款/限速影响。

## 9. 典型场景参数配方

### 9.1 IoT 音频设备（16kHz 单声道 G.711/PCM + 视频 + 文件）

```cpp
cfg.lite_mode            = true;
cfg.lite_profile         = tight::LiteProfile::Video;  // 有视频流：独立 receiver 双线程
cfg.mtu                  = 1350;                 // PCM 40ms 帧 1280B 整包容纳
cfg.flush_interval       = 10ms;                 // lite 自动 ≥10ms
cfg.report_interval      = 500ms;
cfg.late_rtt_multiplier  = 2.5;
cfg.speed_test_enabled   = true;                 // 有视频/文件时需要
cfg.initial_bandwidth_bytes = 4 * 1024 * 1024;   // AIMD 种子（也是提升上限）
cfg.audio_reserved_bps   = 128000;               // 音频编码码率，video_capacity 扣除
cfg.retransmit_enabled   = false;                // 纯实时 AV：在途内存常数化
cfg.fec_enabled          = false;                // 省在途缓冲 + RS 解码 CPU（PLC/req-keyframe 兜底）
// 组包：40ms/帧；优先级 音频2 视频1 文件0；文件块 8KB + 应用层确认
// 视频码率直接跟随协议通知（无需自行折算）：
//   client.set_video_capacity_callback([&](uint64_t bps) { encoder.set_bitrate(bps); });
// 带宽骤降止损（编码器重启后新 IDR 从排空期外正常发送）：
//   client.drain_channel(0);   // 排空视频通道 100ms
```

### 9.2 云端网关（多设备汇聚）

```cpp
cfg.role                 = tight::LinkRole::Node;
cfg.flush_interval       = 2ms;
cfg.queue_limit          = 4096;
cfg.drop_log             = true;
// 其余默认；socket_buffer 8MB 吸收多设备并发突发
```

### 9.3 纯低速传感（只发遥测，极致省电）

```cpp
cfg.lite_mode            = true;
cfg.speed_test_enabled   = false;
cfg.max_message_bytes    = 8 * 1024;             // 下限
cfg.report_interval      = 2000ms;
cfg.heartbeat            = 10000ms;
cfg.retransmit_enabled   = false;                // 遥测丢一包无所谓，内存封顶
```

---

## 10. 外部 socket 模式（UDP/WebSocket/TCP 等自定义通道）

> 完整的集成开发文档（全 API 参考、集成规则、UDP/WebSocket/TCP/内存
> 四个适配案例、常见错误与检查清单）见
> [external_socket_integration.md](external_socket_integration.md)，
> 本节为速览。

默认模式下 tight 自己创建并维护一个 UDP socket。外部 socket 模式相反：
**应用把已有的网络通道适配为 `tight::ISocket` 接口交给 tight**，tight 不再
创建/绑定/关闭任何 socket，也不启动接收线程——适合复用已有连接的场景
（与业务共用一条 WebSocket、在 TCP 隧道/QUIC 流/进程间通道上跑 tight、
或宿主已有 UDP socket 需要统一管理）。

模型：

- **发送（拉）**：tight 的全部出站报文（握手/心跳/报告/数据）经
  `ISocket::send_to()` 输出；
- **接收（推）**：应用在自己的收包循环里拿到一个完整 tight 数据报后，调
  `TightTransport::inject_packet()` 注入（在调用线程内同步完成解码与分发）。

### 10.1 适配示例（以 WebSocket 风格通道为例）

```cpp
#include "tight/tight.hpp"

// 把已有通道适配为 ISocket。send_to 会被 tight 内部多个工作线程并发
// 调用：实现必须线程安全、不得长阻塞（失败返回 false，报文按丢弃处理，
// 可靠通道由 ARQ 重传兜底）。
class WsSocket : public tight::ISocket {
public:
    explicit WsSocket(MyWsConnection& conn) : m_conn(conn) {}

    bool send_to(const tight::NetAddress& /*to*/,
                 const std::uint8_t* data, std::size_t size) override {
        // 单连接通道忽略 to；WebSocket 二进制消息自带分帧边界。
        // TCP 等流式通道需在此自行加分帧（如 2 字节长度前缀），接收侧
        // 拆帧后每个完整 tight 数据报调一次 inject_packet。
        return m_conn.send_binary(data, size);
    }
private:
    MyWsConnection& m_conn;
};

// 组装
auto sock = std::make_shared<WsSocket>(ws_conn);
tight::TightConfig cfg;
cfg.id = "device-1";
tight::TightTransport transport(cfg, sock);   // 外部 socket 模式
transport.set_message_callback([](const std::string& peer, tight::Bytes payload) {
    // ...
});
transport.start();

// 应用自己的收包循环：收到完整 tight 数据报 → 注入
ws_conn.on_binary([&](const uint8_t* data, size_t size) {
    // from 填对端 connect() 时的逻辑地址（单连接通道可固定值）
    transport.inject_packet(tight::NetAddress{"127.0.0.1", 9000}, data, size);
});

transport.connect(tight::RemotePeer{"server", tight::NetAddress{"127.0.0.1", 9000}});
```

### 10.2 行为约定

- tight 报文自带 48B 头 + CRC、按数据报自描述；**流式通道的分帧由适配层
  负责**（WebSocket 消息天然分帧，TCP 需长度前缀）。
- `send_to()` 的 `to` 与 `inject_packet()` 的 `from` 使用同一套逻辑地址：
  对多对端通道（如共享 UDP socket），地址用于区分对端，须与
  `connect()` 时的 `RemotePeer::address` 一致；单连接通道可忽略。
- 外部 socket 由应用拥有：`stop()`/析构不会关闭它；应用须保证它存活至
  tight 实例停止之后（停止前先停注入）。
- `local_port()` 在外部模式下返回 `config.bind.port`（仅标识，无真实绑定）。
- 其余 API（握手、加密、FEC、拥塞控制、各通道与回调）与默认模式完全一致。

## 11. 典型调用流程（速查）

### 11.1 连接生命周期

```mermaid
sequenceDiagram
    participant App as 宿主应用
    participant T as TightTransport
    participant Peer as 对端 tight

    App->>T: TightConfig + 构造
    App->>T: set_message_callback / 其他回调
    App->>T: start()
    App->>T: connect(remote)
    T->>Peer: 握手（id/token/X25519/能力标志）
    Peer-->>T: HandshakeAck
    T->>Peer: Online（幂等重发）
    Note over T,Peer: 数据/报告/心跳…
    App->>T: send() / send_channel() / send_file() / send_command()
    T->>Peer: Data/Parity（加密+FEC）
    Peer-->>T: Report（ack/NACK/带宽/CE）
    App->>T: stop()
```

### 11.2 发送与码率联动

```mermaid
flowchart TD
    A["应用编码视频帧"] --> B["send_channel(0, 帧)"]
    B --> C{"btl 充足?"}
    C -- 是 --> D["令牌桶放行 → 发送"]
    C -- 否 --> E{"视频通道令牌贷款?"}
    E -- 是 --> F["透支 loan_seconds 发送"]
    E -- 否 --> G["排空窗口/止损"]
    G --> H["video_capacity_bps 回调<br/>应用降低编码码率"]
    H --> A
```

## 12. 故障排查与常见问题（FAQ）

| 现象 | 原因与处理 |
|---|---|
| 对端一直收不到消息 | 检查 `token`/`id` 一致性、握手是否完成（`peer_online` 查询）；确认 `role` 配置（Node 必须服务端） |
| 视频卡顿但音频正常 | 检查 `video_capacity_bps` 回调是否被正确消费（编码码率应随回调调整）；确认 `audio_reserved_bps` 已配置 |
| 大量 `clear_outbound()` 止损 | 出站积压 >3000 或 sendFail 触发：检查对端 `dead_timeout`/`heartbeat` 配置与链路质量 |
| `LoanExhaustedCallback` 频繁触发 | `loan_seconds` 过小或编码码率超预算：提高 loan 或降低码率，确认音频预留正确 |
| 弱网下吞吐骤降 | FEC 冗余关闭（RTT>200ms/CE>1%）后靠 ARQ：确认应用层容错（PLC/req-keyframe）就位 |
| lite 模式队列溢出静默丢包 | `queue_limit≤128`/`encode≤64`/`outbound≤256` 容量收紧：检查发送速率与消费速率匹配 |
| 外部 socket 模式收不到包 | 确认 `inject_packet()` 的 `from` 地址与 `connect()` 的 `RemotePeer::address` 一致 |
| 内存不降 | 确认无重传场景 `retransmit_enabled=false`（在途为常数）；队列容量钳制是否生效 |

### 12.1 排查流程

```mermaid
flowchart TD
    S["消息收发异常"] --> Q1{"握手完成?"}
    Q1 -- 否 --> A1["查握手日志/超时配置/ token 一致性"]
    Q1 -- 是 --> Q2{"发送侧队列积压?"}
    Q2 -- 是 --> A2["查 send_queue/outbound 深度，调整码率/队列"]
    Q2 -- 否 --> Q3{"接收侧缺口/FEC 丢失?"}
    Q3 -- 是 --> A3["查 Report 迟到率/丢包率，确认 FEC 冗余与 ARQ"]
    Q3 -- 否 --> A4["查回调注册与线程模型，确认应用侧消费"]

