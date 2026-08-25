# Lite Mode 内存优化思路与业务逻辑方案

## 1. 内存模型与基准

| 指标 | 数值 | 说明 |
|---|---|---|
| 进程地板 | ~5.5MB | CRT/系统驻留，与协议栈无关 |
| tight lite 实例（空闲） | **~76KB** | 单连接 peer 全量状态 + 队列骨架 |
| 稳态传输 delta | ~136KB | 50KB/s × 8KB 消息；delta ≈ 在途字节，与总流量无关 |
| 重传关闭时在途内存 | ~24KB 常数 | 不随码率增长 |

核心方法论：**delta ≈ 在途字节**；多轮收发曲线呈平台期即无泄漏（CRT 高水位保留属正常，
free 不还 OS）。测量工具：`GetProcessMemoryInfo` + `tests/test_mem8k`。

## 2. 已实现的优化（按收益排序）

### 2.1 结构性优化

| 优化 | 收益 | 位置 |
|---|---|---|
| **线程 4→1** | 省 3 个线程栈（Win 默认各 1MB 保留）+ TCB | transport.cpp:334-341/473-477 |
| **64KB 小栈线程** | 栈保留 1MB→64KB，降 94% | small_thread.hpp:62-82（`_beginthreadex` / `pthread_attr_setstacksize`） |
| **队列容量钳制** | encode ≤64 / outbound ≤256 / queue ≤128，封顶队列骨架内存 | transport.cpp:116-138 |
| **socket 内核缓冲 ≤16KB** | SO_RCVBUF/SO_SNDBUF 各 8MB→16KB | transport.cpp:301-303 |
| **重传协商关闭** | 在途内存 ∝码率 → 常数 ~24KB | report.cpp:46-52、transport.cpp:1049-1050 |

### 2.2 分配策略优化

| 优化 | 说明 | 位置 |
|---|---|---|
| **buffer_pool** | thread_local 2048B 固定块自由链表，每线程 ≤16 块（32KB），无锁、跨线程释放安全 | buffer_pool.hpp |
| **PooledBytes 出站报文** | 出站报文走池化块，发送路径零 malloc | transport.cpp:82 |
| **单缓冲报文构建** | `build_wire_packet`：头→密文→CRC 一次分配，AES 直接密写进负载区 | transport.cpp:982-1005 |
| **栈缓冲解码** | drain_receiver 2048B 栈缓冲直接解码，无堆分配 | transport.cpp:502 |
| **Span 零拷贝分片** | Fragmenter 以 `ReedSolomon::Span` 视图引用唯一整体缓冲；`encode_into` + thread_local 缓冲复用 | fragmenter.cpp:48-70 |
| **流式 CRC** | 头 44B + 4 零字节 + 负载顺序喂 CRC，免拼接临时缓冲 | packet_codec.cpp:118-123 |
| **BlockingQueue 节点回收池** | 节点复用上限 64 | blocking_queue.hpp:128-138 |

### 2.3 防御性上限（防内存被对端/异常流量打爆）

`max_message_bytes` [8KB,10MB]、`fragment_count` 上限、`m_missing_seqs` ≤4096、
drop 日志限流——详见 [04_security.md](04_security.md) §5。

## 3. 进一步优化思路（候选，按投入产出排序)

### 3.1 高收益

1. **message_id/seq 独立不变量已消除幽灵序号泄漏**——维护重点是回归测试守护，
   任何改动不得让 msg_id 占用数据序列空间（peer.hpp:53-57）。
2. **重组缓冲按 peer 限额**：当前 `m_incoming` 上限由 `max_message_bytes` 间接约束；
   可增加「全 peer 重组字节总额」预算，超额按 LRU 驱逐未完成消息，
   防止多消息并发重组叠加（lite 单连接场景收益有限，网关侧收益大）。
3. **缺包时立即释放已聚合尾部**：消息缺口被 3.5×RTT 跳过后，已收分片可提前回收，
   不必等完整组装超时。

### 3.2 中收益

4. **flush ≥10ms 与「空闲深睡」结合**：连续 N 拍无收发时指数退避节拍（10ms→50ms），
   有报文立即恢复——进一步降 CPU 唤醒（对内存无直接收益，但省电）。
5. **buffer_pool 块大小分档**：当前统一 2048B；按 mtu 分档（如 1408/2048）减少块内浪费；
   或对遥测场景提供 512B 档。
6. **Peer 状态稀疏化**：测速器、AIMD 估计器等在功能关闭时以 `optional` 惰性构造，
   空闲实例可再省数 KB。
7. **心跳自适应**：链路稳定后心跳 1s→10s 退避（usage.md 9.3 已手动给出配方，
   可内置为 `heartbeat_adaptive`）。

### 3.3 低收益 / 不建议

8. ~~自定义全局分配器~~：CRT 高水位语义下收益有限且引入复杂度；
9. ~~压缩日志~~：lite 已强制静默；
10. ~~更小的 PacketHeader~~：48B 头改 packed 变长会破坏 CRC/AAD 确定性设计与兼容。

## 4. 业务逻辑方案（应用层配合）

协议栈只保证「内存有界」，业务侧需按场景选择正确配方：

### 4.1 实时音视频（usage.md 9.1）

- `retransmit_enabled=false`：在途内存常数 ~24KB，迟到帧直接丢弃；
- mtu=1350 单帧单包：无分片/重组内存，无重组延迟；
- FEC 保留：熵驱动 1-3 校验片，用固定小冗余换重传关闭后的抗丢包。

### 4.2 文件/固件传输

- 协议重传上限 10 次耗尽即静默丢弃 → **必须应用层分块**：
  - 按块（如 64KB）编号 + 块级 CRC/SHA 校验；
  - 收端维护缺失块位图，整轮收完后 NACK 补发缺失块（类 Block ACK 的二次确认）；
  - 发端限速（≤ 链路带宽估算值），避免洪泛触发内核缓冲溢出丢包。
- 用 `send_command` 做块位图控制面（单报文、插队、保序），数据面走普通 send。

### 4.3 低速遥测（usage.md 9.3）

- `max_message_bytes=8KB`、关测速、心跳 10s；
- 业务侧攒批上报，减少消息数（每条消息有 msg/分片状态开销）。

### 4.4 限速纪律（内存测试与生产的共同前提）

- 数据面洪泛必丢（socket 缓冲溢出 + Report 本身也会丢）：
  **内存敏感场景必须限速**（如 50KB/s）或自适应等待 received 追上 sent；
- 生产侧可用 `BandwidthEstimator::bytes_per_second()` 做发端 pacing 上界。

### 4.5 网关侧配合（普通模式端）

- 网关保持 4 线程 + 8MB socket 缓冲吸收多设备突发；
- 网关对 lite 设备**降速下发**：按设备 report 回来的带宽 pacing，
  避免打爆设备 16KB 内核缓冲（丢包对 lite 端即内存压力与重传放大）；
- 网关在黑板层聚合设备指标，发现某设备迟到率持续 > 阈值时提示其切纯 FEC 模式。

## 5. 验收与回归

1. `test_mem8k`（单进程）+ `test_mem8k_split`（双进程）曲线平台期验证；
2. 10/10 ctest 全绿后才允许提交；
3. 每次涉及 peer 状态、队列、重传路径的改动，必须重跑内存基准并对比
   136KB 稳态 delta 是否漂移。
