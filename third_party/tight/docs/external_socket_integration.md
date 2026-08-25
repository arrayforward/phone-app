# tight 外部 socket 注入模式 —— 集成开发文档

> 目标读者：需要把 tight 跑在**已有网络通道**上的集成开发者（人类或 AI）。
> 本文是自包含的集成参考：核心模型、完整 API、必须遵守的规则、四个
> 可直接照搬的适配案例、常见错误与检查清单。
> 公共头文件：`include/tight/socket.hpp`、`include/tight/tight.hpp`、
> `include/tight/types.hpp`。参考实现：`tests/test_external_socket.cpp`
> （内存回环）与 `tests/e2e_harness.hpp`（带丢包/延迟脚本的可编程链路）。

---

## 目录

- [1. 这个模式是什么](#1-这个模式是什么)
- [2. 核心模型（先理解再动手）](#2-核心模型先理解再动手)
- [3. 快速开始（最小完整示例）](#3-快速开始最小完整示例)
- [4. 完整 API 参考](#4-完整-api-参考)
- [5. 集成规则（必须逐条遵守）](#5-集成规则必须逐条遵守)
- [6. 适配案例](#6-适配案例)
- [7. 常见错误（FAQ）](#7-常见错误faq)
- [8. 集成检查清单](#8-集成检查清单)

---

## 1. 这个模式是什么

tight 默认自己创建、绑定并维护一个 UDP socket（"内部 socket 模式"）。
**外部 socket 注入模式**相反：应用把自己已有的网络通道——已建立的
UDP socket、WebSocket 连接、TCP 连接、QUIC 流、隧道、进程间队列等——
适配成 `tight::ISocket` 接口交给 tight，此后：

- tight **不创建、不绑定、不关闭任何 socket**；
- tight **不启动接收线程**（普通模式有 receiver 线程）；
- 协议全部能力（握手、X25519+AES-256-GCM 加密、FEC、ARQ 重传、
  AIMD 拥塞控制、多通道、文件/数据/命令通道）原样保留。

适用场景：

- 宿主已有一条连接（如与业务共用的 WebSocket），不想再开 UDP 端口；
- 运行环境不允许直接操作 socket（容器/沙箱/某些 IoT SDK）；
- 需要穿过只支持 TCP 的隧道/代理；
- 多个 tight 实例或与其它协议**复用同一条传输通道**；
- 测试：用内存链路做确定性协议测试（本仓库的 e2e 测试即如此）。

## 2. 核心模型（先理解再动手）

### 2.1 数据流：发送靠"拉"，接收靠"推"

```
tight 内部                        你的适配层                     对端
─────────────  发送  ───────────────────────────────────────────►
TightTransport ──send_to(to,data,size)──► ISocket 实现 ──► 你的通道
（reactor/encode/sender 线程调用）      （UDP sendto / WS 发送 /
                                        TCP 写+分帧 / 队列 push）

─────────────  接收  ◄───────────────────────────────────────────
TightTransport ◄──inject_packet(from,data,size)── 你的收包循环
（在你的调用线程内同步解码+分发+触发回调）   （WS on_message / TCP 拆帧 /
                                            UDP recvfrom / 队列 pop）
```

- tight 的每一个出站报文（握手/心跳/报告/Ack/数据分片）都是一个**完整的
  tight 数据报**（48 字节头 + 载荷 + CRC32，自描述、自校验），经
  `ISocket::send_to()` 交给你的通道。
- 你的收包侧每拿到**一个完整 tight 数据报**，调一次
  `TightTransport::inject_packet()`。解码、校验、解密、重组、状态机推进、
  应用回调全部在 `inject_packet()` 的**调用线程内同步完成**。

### 2.2 职责分工

| 职责 | 谁负责 |
| --- | --- |
| tight 协议逻辑（可靠性/加密/拥塞控制/通道） | tight 库 |
| 数据报分帧（流式通道） | **你的适配层** |
| 实际收发字节 | **你的通道** |
| 收包线程 | **你的线程**（tight 不起接收线程） |
| 发送线程 | tight 内部线程（调你的 `send_to`） |
| 外部 socket/连接的生命周期 | **你**（tight 不关闭它） |

### 2.3 报文边界是铁律

`inject_packet()` 的每次调用必须对应**恰好一个完整 tight 数据报**：

- UDP / WebSocket：天然按报文/消息分帧，一帧 = 一次 inject；
- TCP 等流式通道：**必须在发送侧加分帧头**（推荐 2 字节大端长度前缀），
  接收侧按前缀拆帧后再 inject——粘包/半包由你的拆帧代码处理；
- 一个 tight 数据报最大 ~2048 字节（默认 MTU 1350 + 头 + GCM tag），
  按 2048 字节预留缓冲区即可覆盖所有配置。

## 3. 快速开始（最小完整示例）

下面是一个**可以直接编译运行**的最小例子：两个 tight 实例通过内存
队列互通。它展示了外部模式的全部要素：实现 `ISocket`、构造注入、
启动、connect、收发、停止。

```cpp
#include "tight/tight.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// 1) 实现 ISocket：把 tight 的出站数据报投递给对端的 inject_packet。
//    真实项目里这里换成你的 UDP sendto / WebSocket send / TCP write。
class PipeSocket : public tight::ISocket {
public:
    ~PipeSocket() override { stop(); }

    // target = 对端 transport；from = 本端逻辑地址（对端据此识别来源）
    void attach(tight::TightTransport* target, tight::NetAddress from) {
        std::lock_guard<std::mutex> lk(mu_);
        target_ = target;
        from_ = std::move(from);
    }
    void start() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            running_ = true;
        }
        thread_ = std::thread([this] { pump(); });
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            running_ = false;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    // tight 内部线程调用：必须线程安全、不得阻塞。
    bool send_to(const tight::NetAddress& /*to*/, const std::uint8_t* data,
                 std::size_t size) override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!running_) return false;
            queue_.emplace_back(data, data + size);
        }
        cv_.notify_one();
        return true;
    }

private:
    void pump() {   // 收包循环：每个完整数据报 inject 一次
        for (;;) {
            std::vector<std::uint8_t> d;
            tight::TightTransport* target;
            tight::NetAddress from;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return !running_ || !queue_.empty(); });
                if (!running_ && queue_.empty()) return;
                d = std::move(queue_.front());
                queue_.pop_front();
                target = target_;
                from = from_;
            }
            if (target) target->inject_packet(from, d.data(), d.size());
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::vector<std::uint8_t>> queue_;
    tight::TightTransport* target_{nullptr};
    tight::NetAddress from_;
    bool running_{false};
    std::thread thread_;
};

int main() {
    using namespace tight;

    // 2) 配置：id 必填；bind.port 在外部模式只作逻辑标识（无真实绑定）
    TightConfig cfg_a;
    cfg_a.id = "A";
    cfg_a.bind = NetAddress{"127.0.0.1", 10001};
    cfg_a.speed_test_enabled = false;      // 示例从简：跳过测速列车
    TightConfig cfg_b = cfg_a;
    cfg_b.id = "B";
    cfg_b.bind = NetAddress{"127.0.0.1", 10002};

    // 3) 外部 socket 模式构造
    auto sock_a = std::make_shared<PipeSocket>();
    auto sock_b = std::make_shared<PipeSocket>();
    TightTransport a(cfg_a, sock_a);
    TightTransport b(cfg_b, sock_b);

    b.set_message_callback([](const std::string& peer, Bytes payload) {
        std::string s(payload.begin(), payload.end());
        std::cout << "B received from " << peer << ": " << s << "\n";
    });

    // 4) 交叉接线：A 发出的注入 B（来源=A），B 发出的注入 A（来源=B）
    sock_a->attach(&b, NetAddress{"127.0.0.1", 10001});
    sock_b->attach(&a, NetAddress{"127.0.0.1", 10002});
    sock_a->start();
    sock_b->start();

    // 5) 启动 tight（外部模式：不建 socket、不起接收线程）
    if (!a.start() || !b.start()) return 1;

    // 6) A 主动连接 B（地址 = 逻辑地址，须与 inject 的 from 一致）
    a.connect(RemotePeer{"B", NetAddress{"127.0.0.1", 10002}});

    // 7) 等链路起来后发送（payload 首字节避开 0x01..0x03，见 5.6）
    for (int i = 0; i < 200; ++i) {
        bool up = false;
        for (const auto& ev : a.peers()) {
            if (ev.id == "B" && (ev.state == LinkState::Established ||
                                 ev.state == LinkState::Online)) up = true;
        }
        if (up) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Bytes msg{'h', 'e', 'l', 'l', 'o'};
    a.send("B", Bytes(msg));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // 8) 停止顺序：先停注入（socket），再停 transport
    sock_a->stop();
    sock_b->stop();
    a.stop();
    b.stop();
    return 0;
}
```

编译（接入方式与默认模式相同）：

```cmake
add_subdirectory(tight)
target_link_libraries(your_app PRIVATE tight)   # Windows 已自动链 ws2_32
```

## 4. 完整 API 参考

### 4.1 `tight::ISocket`（`tight/socket.hpp`）

你要实现的唯一接口，只有一个方法：

```cpp
class ISocket {
public:
    virtual ~ISocket() = default;
    virtual bool send_to(const NetAddress& to,
                         const std::uint8_t* data, std::size_t size) = 0;
};
```

| 要素 | 约定 |
| --- | --- |
| 调用方 | tight 内部任意工作线程（reactor/encode/sender），**多线程并发** |
| 线程安全 | **必须由实现保证**（典型做法：内部加一把互斥锁） |
| 阻塞 | **不得长时间阻塞**。通道写不动时应立即返回 `false`，不要 sleep 重试 |
| 返回值 | `true` = 已接收（不保证送达）；`false` = 发送失败，该报文被 tight 丢弃（可靠通道由 ARQ 重传兜底，偶发失败无害） |
| `to` | 对端的**逻辑地址**（= 对端 `connect()` 时 `RemotePeer::address` 的值）。单连接通道（已建立的 WebSocket/TCP）**可忽略**；多对端共享通道（如一个 UDP socket 服务多个 tight 对端）必须用 `to` 区分目的 |
| `data/size` | 一个完整 tight 数据报，最大约 2048 字节；指针仅在调用期间有效（需要异步发送时必须拷贝） |

### 4.2 构造：`TightTransport`

```cpp
explicit TightTransport(TightConfig config);                              // 默认（内部 UDP socket）
TightTransport(TightConfig config, std::shared_ptr<ISocket> socket);      // 外部 socket 模式
```

- `socket` 非空 → 外部模式；`socket == nullptr` → 与默认构造完全等价。
- tight 以 `shared_ptr` 持有该 socket 直到自身析构；**不要在 tight
  `stop()`/析构之前销毁它**。
- 一个 `ISocket` 实例只应绑定给一个 `TightTransport`（除非你自己做多路
  分发，见案例四的多对端讨论）。

### 4.3 接收注入：`inject_packet`

```cpp
void inject_packet(const NetAddress& from, const std::uint8_t* data, std::size_t size);
void inject_packet(const NetAddress& from, const Bytes& datagram);   // 便捷重载
```

| 要素 | 约定 |
| --- | --- |
| 何时调 | 你的收包循环每拿到一个完整 tight 数据报时 |
| 调用线程 | 任意线程（通常是你的 IO 线程）；tight 在该线程内**同步**完成解码/解密/重组/状态机/应用回调 |
| `from` | 来源对端的逻辑地址：必须与该对端 `connect()` 的 `RemotePeer::address` **完全一致**（host 字符串 + port），否则会被识别成一个新对端。单连接通道固定填对端地址即可 |
| `data/size` | 完整数据报（含 48B 头）。不完整/CRC 错误/畸形的报文会被静默丢弃——所以分帧错误表现为什么都收不到 |
| `size` 上限 | 内部接收缓冲 2048B，超过的数据报进不来（正常配置下 tight 自身不产生超过 2048B 的报文） |
| 时序 | `start()` 之后调用才有效；`stop()` 之后调用直接返回；默认（内部 socket）模式下调用无效 |
| 重入 | 可以在 `send_to()` 里**同步**回调对端的 `inject_packet()`（内存直连场景），但要自行评估栈深度；推荐异步 pump（如快速开始示例） |

### 4.4 生命周期：`start` / `stop` / 析构

| 方法 | 外部模式行为 |
| --- | --- |
| `bool start()` | **不创建 socket、不绑定、不设置 sockopt、不起接收线程**；只启动协议线程（reactor/encode/sender/cap 通知；lite 模式相应合并）。几乎不会失败（仍可能因线程创建失败返回 false） |
| `void stop()` | 发 Bye（经你的 `send_to`）、停止全部协议线程、幂等。**不会关闭/断开你的通道** |
| `~TightTransport()` | 等价于自动 `stop()`（先发 Bye） |

推荐停止顺序：**先停止注入（你的收包循环/pump），再 `stop()` transport**。
反过来也安全（`inject_packet` 在 stop 后是空操作），但可能丢失在途报文。

### 4.5 连接：`connect`

```cpp
bool connect(const RemotePeer& remote);   // remote = {id, NetAddress{host, port}}
```

- 与默认模式语义相同：登记对端、进入握手（X25519 密钥交换在握手内完成）。
- `remote.address` 是**逻辑地址**：对端 inject 你时填的 `from` 必须等于它。
  建议直接使用对端 `TightConfig::bind` 里的值（只是一种命名约定，无网络语义）。
- 服务端角色（被动接受连接）不需要 connect：任何来源的第一个报文会自动
  创建匿名对端（id 形如 `anon-<port>-<rand>`），握手后可通过 `peers()`
  或 `set_peer_callback` 拿到这个 id 用于回发。

### 4.6 发送 API（与默认模式完全一致）

| 方法 | 说明 | 返回 false 条件 |
| --- | --- | --- |
| `bool send(peer_id, Bytes)` | 数据消息，通道 0 | 未 start / 队列满 / 超 `max_message_bytes` |
| `bool send_video(peer_id, Bytes, bool keyframe)` | 视频帧（通道 0），`keyframe=true` 标记 IDR | 同上 |
| `bool send_channel(peer_id, Bytes, uint8_t channel)` | 指定逻辑通道 0..7 | 同上 |
| `bool send_priority(peer_id, Bytes, int priority)` | 高优先级先出队 | 同上 |
| `bool send_file(peer_id, name, Bytes)` | 文件通道（=2，需两端 `channel_reliable[2]=true`），块级 ARQ，收端 `set_file_callback` 交付 | name 超 65535 / 队列背压 |
| `bool send_data(peer_id, Bytes)` | 可靠数据通道（=3，需两端 `channel_reliable[3]=true`），消息级去重 exactly-once，收端 `set_data_callback` 交付 | 队列背压 |
| `bool send_command(peer_id, Bytes)` | 命令通道：单报文（≤ MTU−48）、保序、插队直发 | 未 Established/Online / 超单包载荷 |

**重要**：对不存在的 `peer_id` 调用上述接口**返回 true**（消息入队即
true，之后在管线里因找不到对端被静默丢弃）。发送前请用 `peers()` 或
peer 回调确认对端存在。

**首字节陷阱**：`send()`/`send_channel()`/`send_priority()` 的应用消息
**首字节必须避开 `0x01`、`0x02`、`0x03`**——接收端按首字节把这类消息
拦截为 file/data 通道内部报文（文件清单/文件块/可靠数据），你的消息会
被静默吞掉。二进制协议建议首字节 ≥ 0x10。

### 4.7 回调注册 API（与默认模式相同，注意触发线程）

| 方法 | 触发时机 | 外部模式触发线程 |
| --- | --- | --- |
| `set_message_callback` | 通用消息重组完成 | **你的 inject 调用线程** |
| `set_peer_callback` | 对端状态迁移（Established/Online/Closed） | inject 线程 或 reactor 线程 |
| `set_command_callback` | 命令按序投递 | inject 线程 或 reactor 线程 |
| `set_message_loss_callback` | 消息经 FEC 仍无法恢复（视频=丢帧，应用据此请求关键帧） | inject 线程 |
| `set_file_callback` | 文件完整接收 | inject 线程 |
| `set_data_callback` | 可靠数据消息（去重后） | inject 线程 |
| `set_video_capacity_callback` | 视频可用码率变化超迟滞 | tight 专用通知线程 |
| `set_loan_exhausted_callback` | 令牌贷款耗尽/恢复 | tight sender 线程 |
| `set_evac_keyframe_callback` | 拥塞排空窗口触发（应重启编码器出 IDR） | inject/reactor 线程 |

回调铁律（与默认模式相同）：**快速返回**——只做加锁/置标志/入队，
**不要**在回调里做 IO、等待、或回调同一个 transport 的发送接口
（`inject_packet` 路径上等于在你的 IO 线程里阻塞协议分发）。

### 4.8 模式与诊断 API

| 方法 | 外部模式说明 |
| --- | --- |
| `set_lite_mode(bool)` / `lite_mode()` | 可用，语义相同（lite 下 reactor 合并 encode/sender；外部模式本就无 receiver） |
| `peers()` | 相同：当前对端快照（id/role/state/client_id） |
| `local_port()` | **返回 `config.bind.port`**（仅标识，无真实绑定） |
| `estimated_bandwidth_bps()` / `btl_bw_bps()` / `video_capacity_bps()` / `fec_redundancy_ratio()` / `pacer_app_limited()` / `pacer_limited()` / `peer_p50_ms()` | 相同，拥塞控制全部照常工作 |
| `outbound_queue_size()` / `clear_outbound()` / `drain_channel(...)` / `file_data_pending_bytes()` | 相同 |

### 4.9 `TightConfig` 在外部模式下的有效字段

完全有效（协议行为）：`id`、`token`、`role`、`mtu`、`heartbeat`、
`report_interval`、`flush_interval`、`dead_timeout`、`retransmit_timeout`、
`initial_bandwidth_bytes`、`queue_limit`、`max_message_bytes`、`drop_log`、
`retransmit_enabled`、`late_rtt_multiplier`、`late_buffer_ms`、
`audio_reserved_bps`、`loan_seconds`、`slowdown_window_ms`、
`channel_fec_extra[8]`、`channel_reliable[8]`、`speed_test_enabled`、
`speed_test_bytes`、`encryption_enabled`、`encode_queue_limit`、
`outbound_queue_limit`、`lite_mode`、`lite_profile`、`fec_enabled`。

外部模式下**被忽略**的字段：

| 字段 | 原因 |
| --- | --- |
| `bind.host` | 不绑定；`bind.port` 仅作 `local_port()`/逻辑地址标识 |
| `socket_buffer_bytes` | 没有内部 socket，无 SO_RCVBUF/SO_SNDBUF 可设 |

注意 `speed_test_enabled`（默认开）：链路上线后会经 `send_to` 突发发送
`speed_test_bytes`（默认 100KB）空探测报文测速。低带宽/按量计费通道
建议设为 `false` 或调小。

## 5. 集成规则（必须逐条遵守）

1. **分帧**：一次 `inject_packet` = 恰好一个完整 tight 数据报。流式通道
   自己加 2 字节长度前缀（见案例三）。
2. **线程安全**：`send_to` 会被多线程并发调用，实现内部必须加锁或无锁化。
3. **不阻塞**：`send_to` 快速返回，失败返回 `false`；`inject_packet` 路径
   上的应用回调同样快速返回。
4. **地址一致**：`connect()` 的 `RemotePeer::address` ≡ 对端 inject 时的
   `from` ≡ 对端 `config.bind` 的值。三者任何不一致都会变成"另一个对端"。
5. **生命周期**：外部 socket 活过 tight 实例；停止顺序 = 先停注入，再 stop。
6. **首字节**：应用消息首字节避开 `0x01/0x02/0x03`（会被误判为 file/data
   内部报文）。
7. **缓冲区**：收发两侧按 ≥2048B 预留单个数据报缓冲。
8. **加密**：默认开启（握手内 X25519 交换）。两端 `encryption_enabled`
   必须一致，否则握手后数据报全部解密失败被丢弃。
9. **可靠通道配置一致**：`channel_reliable[]`、`retransmit_enabled` 语义
   两端要匹配，否则 file/data 通道行为退化。
10. **不要**把同一个 `ISocket` 实例同时交给多个 `TightTransport`
    （多路复用要在适配层自行按地址分发，且协议无连接复用语义，不推荐）。

## 6. 适配案例

### 6.1 案例一：复用已有 UDP socket

场景：宿主进程已经有一个 UDP socket（比如与业务共用端口），希望
tight 走它而不是另开端口。要点：你的接收循环按"数据来源 +  magic
识别"把 tight 报文分流给 inject；非 tight 报文走原业务。

```cpp
class UdpSharedSocket : public tight::ISocket {
public:
    // 宿主已有的非阻塞 UDP socket（fd 由宿主创建/拥有）
    explicit UdpSharedSocket(int fd) : fd_(fd) {}

    bool send_to(const tight::NetAddress& to, const std::uint8_t* data,
                 std::size_t size) override {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(to.port);
        inet_pton(AF_INET, to.host.c_str(), &addr.sin_addr);
        std::lock_guard<std::mutex> lk(send_mu_);       // send_to 多线程并发
        int n = ::sendto(fd_, reinterpret_cast<const char*>(data),
                         static_cast<int>(size), 0,
                         reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        return n == static_cast<int>(size);             // 失败即丢（ARQ 兜底）
    }

    // 宿主的统一收包循环里调用（你自己的线程）：
    void recv_loop(tight::TightTransport& transport) {
        std::uint8_t buf[2048];
        for (;;) {
            sockaddr_in from{};
            socklen_t flen = sizeof(from);
            int n = ::recvfrom(fd_, reinterpret_cast<char*>(buf), sizeof(buf),
                               0, reinterpret_cast<sockaddr*>(&from), &flen);
            if (n <= 0) continue;
            // tight 数据报识别：48B 头，magic = 'TGHT' (0x54474854 大端)
            if (n >= 4 && buf[0] == 'T' && buf[1] == 'G' &&
                buf[2] == 'H' && buf[3] == 'T') {
                char host[INET_ADDRSTRLEN]{};
                inet_ntop(AF_INET, &from.sin_addr, host, sizeof(host));
                transport.inject_packet(
                    tight::NetAddress{host, ntohs(from.sin_port)}, buf,
                    static_cast<std::size_t>(n));
            } else {
                // 原业务报文：交给原有处理逻辑
            }
        }
    }

private:
    int fd_;
    std::mutex send_mu_;
};
```

### 6.2 案例二：WebSocket（单连接，二进制消息）

WebSocket 消息天然分帧，是最省心的通道。单连接场景忽略 `to`/`from`
的具体值（固定即可）。

```cpp
class WsSocket : public tight::ISocket {
public:
    explicit WsSocket(MyWsClient& ws) : ws_(ws) {}

    bool send_to(const tight::NetAddress&, const std::uint8_t* data,
                 std::size_t size) override {
        // 单连接：忽略 to。要求 ws_.send_binary 线程安全（不安全就在此加锁）。
        return ws_.send_binary(data, size);   // 排队即 true，失败 false
    }

    // 在 WebSocket 的 on_message 回调里（库自己的 IO 线程）：
    void on_ws_binary(const std::uint8_t* data, std::size_t size) {
        if (transport_) {
            // from 固定填对端逻辑地址（= 本地 connect 时用的地址）
            transport_->inject_packet(peer_addr_, data, size);
        }
    }

    void bind_transport(tight::TightTransport* t, tight::NetAddress peer) {
        transport_ = t;
        peer_addr_ = std::move(peer);
    }

private:
    MyWsClient& ws_;
    tight::TightTransport* transport_{nullptr};
    tight::NetAddress peer_addr_;
};

// 组装：
//   auto sock = std::make_shared<WsSocket>(ws);
//   tight::TightTransport t(cfg, sock);
//   sock->bind_transport(&t, {"server.internal", 9000});
//   ws.on_binary([sock](auto d, auto n){ sock->on_ws_binary(d, n); });
//   t.start();
//   t.connect({"server", {"server.internal", 9000}});
```

### 6.3 案例三：TCP 隧道（长度前缀分帧）

TCP 是字节流，**必须自行分帧**。推荐 2 字节大端长度前缀
（tight 数据报 ≤2048B，2 字节足够）。

```cpp
class TcpSocket : public tight::ISocket {
public:
    explicit TcpSocket(int fd) : fd_(fd) {}

    bool send_to(const tight::NetAddress&, const std::uint8_t* data,
                 std::size_t size) override {
        if (size > 65535) return false;
        std::uint8_t hdr[2] = {static_cast<std::uint8_t>(size >> 8),
                               static_cast<std::uint8_t>(size & 0xFF)};
        std::lock_guard<std::mutex> lk(mu_);
        return write_all(hdr, 2) && write_all(data, size);
    }

    // 收包侧：在你的读循环里增量拆帧，凑齐一帧 inject 一次
    void on_read(tight::TightTransport& t, const tight::NetAddress& peer) {
        std::uint8_t buf[4096];
        for (;;) {
            ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) break;                       // 连接关闭/错误
            rx_.insert(rx_.end(), buf, buf + n);
            for (;;) {
                if (rx_.size() < 2) break;
                std::size_t len = (rx_[0] << 8) | rx_[1];
                if (rx_.size() < 2 + len) break;     // 半包：等更多数据
                t.inject_packet(peer, rx_.data() + 2, len);
                rx_.erase(rx_.begin(), rx_.begin() + 2 + len);
            }
        }
    }

private:
    bool write_all(const std::uint8_t* p, std::size_t n) {
        while (n > 0) {
            ssize_t w = ::send(fd_, reinterpret_cast<const char*>(p),
                               static_cast<int>(n), 0);
            if (w <= 0) return false;
            p += w; n -= static_cast<std::size_t>(w);
        }
        return true;
    }
    int fd_;
    std::mutex mu_;
    std::vector<std::uint8_t> rx_;   // 拆帧累积缓冲
};
```

注意：TCP 无丢包但会**排队积压**——tight 的 AIMD 拥塞控制依赖延迟/
迟到信号，在 TCP 上依然工作（延迟上升 → 降速），但"丢包驱动"的
NACK/FEC 路径不会被触发，属于正常现象。

### 6.4 案例四：进程内 / 测试通道

即本文 [快速开始](#3-快速开始最小完整示例) 的 `PipeSocket`：队列 +
pump 线程。仓库中还提供了带**确定性丢包/重复/延迟/乱序**脚本能力的
版本（`tests/e2e_harness.hpp` 的 `ScriptedSocket`），可直接用于你自己
的集成测试。

## 7. 常见错误（FAQ）

1. **什么都收不到** → 90% 是分帧错误（inject 了半个/粘了多个数据报）
   或 `from`/`connect` 地址不一致。先打印每次 inject 的 size 与首 4
   字节（应为 `TGHT` magic）。
2. **握手成功但应用消息丢** → 检查消息首字节是不是 `0x01/0x02/0x03`
   （被 file/data 内部通道拦截）。改成 ≥0x10。
3. **`send` 返回 true 但对面没反应** → `peer_id` 写错了（不存在的 id
   返回 true 后静默丢弃）。用 `peers()` 确认。
4. **回调里调用 transport 方法导致死锁/卡顿** → 回调在 inject 线程内
   同步执行，必须快速返回；把活丢给自己的队列。
5. **send_to 里直接同步 inject 回对端导致深递归** → 内存直连可以这么
   玩，但握手+ACK 链会嵌套多层；推荐异步 pump。
6. **TCP 上一开始一切正常、大流量后延迟飙升** → TCP 的可靠有序语义
   与实时场景本质冲突（队头阻塞）；实时音视频应优先 UDP/WebSocket
   （datagram 语义）或评估 QUIC。
7. **两端加密/可靠通道配置不一致** → 表现为一侧全丢。核对
   `encryption_enabled`、`channel_reliable[]`。
8. **忘记 `speed_test_enabled`** → 上线瞬间 100KB 探测突发打到你的
   通道。按需关闭或调小。

## 8. 集成检查清单

把下面每一项核对完，集成基本不会出问题：

- [ ] `ISocket::send_to` 线程安全（内部加锁）、快速返回、失败返回 false
- [ ] 分帧方案确定：UDP/WS 用天然边界；TCP 已加长度前缀
- [ ] 收包循环里：每个完整数据报 → 一次 `inject_packet`
- [ ] `connect` 地址 ≡ 对端 inject 的 `from` ≡ 对端 `config.bind`
- [ ] `start()` 之后才开始 inject；停止时先停注入再 `stop()`
- [ ] 应用消息首字节 ≥ 0x10
- [ ] 收发缓冲 ≥ 2048B
- [ ] 两端 `encryption_enabled` / `channel_reliable[]` 一致
- [ ] 回调全部快速返回（只置标志/入队）
- [ ] 按需关闭 `speed_test_enabled`
- [ ] 外部 socket 生命周期 > tight 实例
