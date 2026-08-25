# tight — 架构总结

tight 的分层架构、线程模型、模块划分与核心数据流。实现主体在 `src/`（命名空间 `tight::tight_detail`，按职责分目录：`core/` 传输编排与每对端状态、`channel/` 数据通道、`protocol/` 线格式、`fec/` 擦除码、`crypto/` 加密、`net/` 系统网络层、`util/` 基础设施），公共 API 在 `include/tight/`。
设计决策与权衡见 [tight_design.md](tight_design.md)，完整 API 见 [api_reference.md](api_reference.md)。

## 1. 分层架构

```mermaid
flowchart TB
    subgraph L0["应用层"]
        A1["video-sender / video-player"]
        A2["tight-test / test-file-data"]
        A3["宿主应用"]
    end
    subgraph L1["公共 API (include/tight)"]
        B1["tight.hpp — TightTransport 主接口<br/>set_message_callback / send / send_file / send_data /<br/>send_command / set_lite_mode / clear_outbound ..."]
        B2["types.hpp — TightConfig / PacketType / PacketHeader / PeerEvent"]
        B3["packet_codec.hpp — 线格式编解码 + CRC32"]
        B4["fec.hpp — ReedSolomon 擦除码"]
        B5["bandwidth.hpp — 三信号 AIMD 估计器"]
    end
    subgraph L2["核心实现 (src, tight_detail)"]
        C1["core/transport.cpp — TightTransport::Impl<br/>线程模型/状态机/收发/握手/加密接线/通道"]
        C2["channel/reassembler.cpp — 分片重组 + 缺口跟踪 + FEC 恢复"]
        C3["channel/fragmenter.cpp — 分片 + 动态 FEC 冗余"]
        C4["channel/report.cpp — ACK/NACK 报告构建与处理"]
        C5["channel/command.cpp — 命令通道保序"]
        C6["core/bandwidth.cpp — 三信号 AIMD 估计器"]
        C7["fec/fec.cpp / crypto/crypto.cpp<br/>protocol/packet_codec.cpp + crc32.cpp"]
        C8["net/address.cpp / wsa.cpp / socket_platform.hpp / ecn_platform.hpp"]
    end
    subgraph L3["基础设施 (util)"]
        D1["core/peer.hpp — 每对端状态 (PendingSend/IncomingMessage/FileRecv/时钟/FEC档位)"]
        D2["util/buffer_pool.hpp — 2048B 出站缓冲池 (thread_local 无锁)"]
        D3["blocking_queue.hpp — 有界阻塞队列"]
        D4["util/small_thread.hpp — 小栈线程"]
        D5["protocol/wire_format.hpp — 大端转换 / 通道编码常量"]
        D6["net/ecn_platform.hpp — L4S CE 标记常量"]
    end
    subgraph L4["系统层"]
        E1["Windows: ws2_32 + WSAStartup"]
        E2["Linux: POSIX sockets"]
    end

    L0 --> L1 --> L2 --> L3 --> L4
    C1 --> C2
    C1 --> C3
    C1 --> C4
    C1 --> C5
    C1 --> C6
```

## 2. 模块职责

| 模块 | 职责 |
| --- | --- |
| `core/transport.cpp` | 唯一入口 `TightTransport::Impl`：线程编排、Peer 状态机、握手/心跳/保活、加密接线、file/data 通道、令牌桶、视频码率通知（`video_capacity_bps`）、按通道排空（`drain_channel`）、诊断接口 |
| `channel/reassembler.cpp` | 接收路径：sequence 缺口跟踪、分片收集、FEC 解码恢复、消息投递、丢帧通知（`on_message_loss`） |
| `channel/fragmenter.cpp` | 发送路径：消息分片、RS 校验片生成、分段 FEC 状态机（stage 0/1/2）、FEC 关闭（RTT>200ms）、实际冗余率统计 |
| `channel/report.cpp` | 周期报告：ack 游标、迟到率、丢失序号、测速带宽、投递率、丢包率、p50、CE 占比；处理端：重传 + ack 剪枝 |
| `channel/command.cpp` | 命令通道：单报文、保序插队、乱序最多等 3×RTT 后跳号 |
| `core/bandwidth.cpp` | **三信号 AIMD 估计器**：delay-based（排队延迟 = P50−RTprop EWMA）+ late-based（帧级迟到率）+ loss-based（令牌受限时替代迟到率）+ CE 直接信号；**突刺门控**（非持续超发不降速）+ **量化降速阶梯**（×0.20~×0.65 按强度）；恢复两步台阶 ×1.5（CE 活跃跳过 FEC 探测）、下限 100kbps、FEC 探测冗余输出、`congested()`/`delay_congested()`/`last_congest_at()` 状态 |
| `crypto/crypto.cpp` | X25519 / SHA-256 / HKDF / AES-256-GCM（纯 C++ 内置实现） |
| `fec/fec.cpp` | Reed-Solomon GF(2⁸) Vandermonde 编码 / 高斯消元解码 |
| `protocol/packet_codec.cpp` | 48B 头编解码 + 流式 CRC 校验（零堆分配变体） |

## 3. 线程模型

```mermaid
flowchart TB
    subgraph Normal["普通模式（5 线程，服务器端）"]
        RT["reactor 线程<br/>握手重发/心跳/Online通告/报告/命令flush/掉线检查/收包调度"]
        RC["receiver 线程<br/>recvfrom → 解码 → handle_packet"]
        EN["encode 线程<br/>process_send_queue 取任务 → 分片+FEC → 加密 → 入出站队列"]
        SD["sender 线程<br/>令牌桶 pacing → sendto"]
        CN["cap 通知线程<br/>视频可用码率回调（独立线程，不阻塞接收）"]
    end
    subgraph Lite["lite 模式（2 线程，IoT 端侧）"]
        RR["reactor 线程<br/>合并 receiver/encode/sender 全部职责<br/>64KB 小栈"]
        CNL["cap 通知线程（共用）"]
    end

    RT --> RC
    RT --> EN
    RT --> SD
    RT -. 运行时切换 set_lite_mode .-> RR
    CN -. 两种模式共用 .-> RT

    m_send_queue["m_send_queue<br/>(priority map<int, deque>)"]
    m_encode_queue["m_encode_queue<br/>(有界阻塞队列)"]
    m_outbound_queue["m_outbound_queue<br/>(有界阻塞队列)"]
    RC --> RT
    EN --> m_outbound_queue
    SD --> m_outbound_queue
```

### 3.1 线程分工

| 线程 | 职责 |
| --- | --- |
| reactor | 周期节拍：握手重发退避（500ms 起步、封顶 5s）、心跳、Online 幂等重发、报告发送（333ms/1s）、命令 flush、dead_timeout 掉线检查、`handle_packet` 分发 |
| receiver | 独占 `recvfrom`，保证收包不被发送阻塞；CE 标记报文计数并计入迟到统计 |
| encode | 独占 CPU 密集的 FEC 编码 + 加密，reactor 保持空闲处理收包 |
| sender | 独占 `sendto`，令牌桶背压不阻塞 reactor/encode |
| cap 通知 | 消费 `m_cap_queue`（容量 4），以独立线程调 `VideoCapacityCallback`（应用回调须快速返回） |

设计原则：**单生产者多消费者**的出站队列 —— reactor/encode 只入队，sender 只出队并 `sendto`，任何 socket 阻塞都不影响协议核心。

### 3.3 外部 socket 模式

构造时传入 `ISocket` 实现（`include/tight/socket.hpp`）后，tight 不创建/
绑定/关闭任何 socket，也不启动 receiver 线程：全部出站报文（含控制报文）
经统一的 `socket_send()` 出口路由到 `ISocket::send_to()`（默认模式则走内部
UDP socket）；入站为推模型——应用收到完整数据报后调 `inject_packet()`，
在应用线程内同步完成解码 + `handle_packet` 分发（等价于 receiver_loop 的
单次迭代）。lite 模式下 reactor 节拍同样跳过 `drain_receiver()`。

### 3.2 lite 模式

- reactor 合并全部职责，receiver/encode/sender 线程被 join 回收（cap 通知线程两种模式共用）；
- 队列容量收紧：`encode≤64`、`outbound≤256`、`queue_limit≤128`、socket≤16KB；
- `flush_interval` 钳制 ≥10ms（省 CPU 唤醒），`drop_log` 强制关闭（静默丢弃）；
- `set_lite_mode()` 运行时切换：先让 reactor 接管再回收线程（反之先清空 `m_lite_pending`）。

## 4. 发送数据流

```mermaid
sequenceDiagram
    participant App as 应用
    participant Impl as TightTransport::Impl
    participant PSQ as process_send_queue (reactor)
    participant ENC as encode_loop
    participant FRAG as fragment_and_send
    participant PKT as send_data_packet
    participant OUT as outbound_queue
    participant SD as sender_loop (sendto)

    App->>Impl: send()/send_channel()/send_file()/send_data()
    Impl->>Impl: send_message() 按优先级入 m_send_queue<br/>(file/data 写入对应通道, 校验 channel_reliable)
    PSQ->>PSQ: 节拍内取消息 → 入 m_encode_queue
    ENC->>ENC: 取任务 → 查 peer → 检查 max_message_bytes
    ENC->>FRAG: fragment_and_send(peer, payload, channel)
    FRAG->>FRAG: 分片 + 计算 data_count +<br/>分段FEC状态机确定 parity_count<br/>+ channel_fec_extra + probe_extra
    FRAG->>PKT: 每个分片回调 send_data_packet
    PKT->>PKT: 分配 sequence/msg_id、AES-256-GCM 加密、<br/>keep_pending(可靠通道)、构建 48B 头
    PKT->>OUT: PooledBytes 入队
    SD->>SD: 令牌桶 → sendto(对端地址)
```

### 4.1 关键点

- **sequence 与 message_id 分离**：数据序列号 `m_sequence_out` 与消息组 id `m_msg_id_out` 独立计数器，避免对端缺口跟踪出现"幽灵序号"导致 ack 冻结；
- **可靠通道**（`channel_reliable`）：分片保留在 `m_pending` 供 NACK 重传，缺口由对端上报（ackable 控制包挂账在独立的 `m_control_pending`——两序号空间同号常见，混存会让控制 Ack 误删数据挂账导致丢失后永不重传）；
- **音频通道独立队列**（ch1，容量 128）：sender 优先清空且**绕过令牌桶**（实时音频无条件一次性发完，不受贷款/限速影响）；
- **令牌贷款**：视频（ch0）可透支 btl×`loan_seconds`（默认 5s，覆盖编码联动延迟 1~2s），足额消费或贷款透支（不 sleep 打平）；file/data（ch2/3）严格令牌（带宽分配器，不足等下一心跳）；贷款耗尽 → 清空积压 + 持续排空视频通道 + `LoanExhaustedCallback(true)`，债务清零 → 恢复 + 回调 false；
- **FEC 探测冗余**：AIMD 恢复台阶第一步时 `fec_probe_extra()` 追加 2 片校验（仅通道 0，CE 活跃时跳过）压上负载感知链路，第二步确认后移除（业务替换）；
- **FEC 关闭**：平滑 RTT >200ms 或 CE>1%（L4S）→ `peer.m_fec_disable` 置位，fragmenter 校验片全部归零让出带宽，回落自动恢复（丢包型随机丢包不关）；
- **通道排空**（`drain_channel`）：排空期内该通道数据报在 send_queue / encode 队列 / outbound 队列三层出队即丢（不清队列），音频/文件通道不受影响；
- **带宽预算**：`video_capacity_bps = (btl×8 − audio_reserved_bps×(1+channel_fec_extra[1]) − file/data 实时速率) / (1 + 实际FEC冗余率)`，变化 >10% 且 >100kbps 经 cap 通知线程回调；
- **止损**：`outbound_queue_size()>3000` 或 sendFail 1s≥5 → `clear_outbound()`（保留音频）+ 应用侧 force_keyframe + `set_bitrate(500k)`（冷却 2s）。

## 5. 接收数据流

```mermaid
sequenceDiagram
    participant SD as sender_loop (sendto)
    participant RC as receiver_loop (recvfrom)
    participant HP as handle_packet
    participant HS as 握手/心跳/报告/命令处理
    participant RS as Reassembler::handle_data
    participant DV as deliver_message
    participant CB as 应用回调

    SD->>RC: UDP 数据报
    RC->>RC: recvfrom → PacketCodec::decode<br/>(流式 CRC, 零拷贝)
    RC->>HP: handle_packet(from, header, payload)
    HP->>HP: 按包类型分发
    alt Data / Parity
        HP->>RS: 序列号缺口跟踪 / 迟到统计 / 分片收集
        RS->>RS: try_assemble: 缺片则 FEC 解码<br/>RS 解码失败 → on_message_loss(peer, channel)
        RS->>DV: 组装完整 → deliver_message
        DV->>DV: tag 0x01/0x02/0x03 → file/data 内部处理<br/>否则走通用消息
        DV->>CB: 回调
    else Report
        HP->>HP: 更新迟到率/投递率/测速带宽/p50/CE<br/>剪枝 ack、重传 NACK 缺口
    else Command
        HP->>HP: 保序投递（3×RTT 内等缺口）
    else Report
        HP->>HP: RTT 采样(单程×2) → 三信号 AIMD 评估<br/>(p50/late/ce/pacer) → btl 调整 + FEC 探测冗余<br/>RTT>200ms → 关闭 FEC → 码率变化>10% → 通知线程
    else 握手/心跳/在线/bye
        HP->>HP: 状态机推进 + 时钟对表
    end
```

### 5.1 reassembler 内部

```mermaid
flowchart TD
    P["收到 Data/Parity 分片"] --> S{"已完成消息?<br/>m_completed 查重"}
    S -- "是" --> D["丢弃"]
    S -- "否" --> G{"seq 缺口跟踪 + 迟到统计"}
    G --> C["收集进 m_incoming[msg_id]<br/>(防 total_count 不符/idx 越界)"]
    C --> T{"try_assemble<br/>have == data_count?"}
    T -- "是" --> V["FEC 校验片修复缺失<br/>(RS decode 回填)"]
    V -- "恢复成功" --> F["重组 → deliver_message"]
    V -- "恢复失败" --> L["on_message_loss(peer, channel)<br/>丢帧止损/请求关键帧"]
    T -- "否" --> W["等待更多分片/校验片"]
```

## 6. Peer 状态机

```mermaid
stateDiagram-v2
    [*] --> Closed
    Closed --> Handshake: connect() / 收到握手请求
    Handshake --> Established: 收到 HandshakeAck / 回复握手
    Established --> Online: 收到 Online 通告
    Online --> Established: (Online 幂等重发直至确认)
    Handshake --> Closed: 握手超时 / bye
    Established --> Closed: dead_timeout / bye
    Online --> Closed: dead_timeout / bye
    Online --> Online: 每心跳周期重发 Online (幂等)
```

- **握手重发退避**：500ms 起步、封顶 5s，进入 Handshake 状态时重置；
- **Online 幂等重发**：Established/Online 状态下按心跳周期重发（避免单次 Online 丢包导致对端应用层状态永久停在 Established）；
- **时钟对表**：握手时估 offset = `(remote_tick - local_arrival) - rtt/2`，之后每次心跳再同步漂移。

## 7. 内存与缓冲

```mermaid
flowchart LR
    subgraph Buf["缓冲体系"]
        B1["buffer_pool (2048B thread_local 块池)<br/>出站数据报零堆分配复用"]
        B2["m_outbound_queue (PooledBytes)"]
        B3["m_pending (数据重传缓冲)<br/>m_control_pending (控制挂账)"]
        B4["m_incoming (接收重组, 按 msg_id)"]
        B5["m_files (file 重组上下文)"]
    end
    B1 --> B2
    B3 -. 仅可靠通道 .-> B2
    B4 -. 重组后释放 .-> B2
```

| 场景 | 内存档案 |
| --- | --- |
| 普通模式空闲 | ~460KB |
| lite 模式空闲 | ~76KB |
| lite 无重传在途 | 常数 ~24KB（与码率无关） |
| lite 有重传在途 | ∝ 码率 × 确认窗口，队列封顶最坏 ~5.4MB |
