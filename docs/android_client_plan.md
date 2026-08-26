# Android 客户端 + 三模式（single / double / layer）实施方案

> 状态：**已落地并联调通过**（2026-08-26，小米 10 热点 + 华为 NOH-AN00 实测：
> 基础层解码上屏 100%（fed=out），增强层合成命中率 ~62%（150ms grace 窗口），
> 双 MediaCodec 解码器全通）。联调中发现并修复：SPS 解析 constraint 位宽、
> 编码器静止屏卡死误判死循环、华为 NDK AImageReader 输出面 -10000（改走
> Java MediaCodec buffer 模式）、模式选择器初始回调误覆盖 server --mode、
> 增强层赶上语义需要 grace 窗口（等 150ms 再决定合成/直出）、
> **残差纹理从未分配**（首残差帧与基础帧同尺寸 → 共用 resize 标志导致
> glTexSubImage2D 写未分配纹理，shader 采到未定义内容 → 闪红；残差纹理独立
> 尺寸跟踪修复）、断链期垃圾残差未清空（断链即 clear）、顶帧拒收断链
> （改保序全解码，上屏侧丢弃）。
> 关联文档：[protocol.md](protocol.md)（线上协议，含 §3.4 v2）、
> [layered_design.md](layered_design.md)（**残差编解码与快速上屏机制详述**）、
> [layer_test_report.md](layer_test_report.md)（增强层选型实验：差分伪装不可行，
> 直偏置硬编可行）、[ycocg_pipeline.md](ycocg_pipeline.md)（double 打包设计）。

## 1. 目标

1. 新增 **Android APK 客户端**（C++ 核心 + Java 上层薄壳），并行于 Windows 客户端。
2. 协议升级为三种**运行时可切换**模式：

   | 模式 | 编码帧 | 说明 |
   |---|---|---|
   | single | W×H BT.601 4:2:0 | 直接编码（baseline） |
   | double | 2W×H 双 YUV420（raw / YCoCg） | 现状，4:4:4 over 4:2:0 |
   | layer | 基础层 = 低码率 **single** H.264（ch 0）+ 增强层 = 残差熵编码（ch 4） | 两层赶上上屏→合成；赶不上→上屏基础帧 |

3. Windows 客户端**不动**（协议向后兼容，见 §2.4）。
4. 语言方针：**尽量 C++，Java 只做上层**（UI/GLSurfaceView/AudioRecord/输入转发）。
5. 验证环境：小米（root）= server 并开热点；华为 = client 接热点，连网关 IP（默认 `192.168.43.1:8800`）。

## 2. 协议扩展（protocol.md 同步更新）

### 2.1 视频消息 flags（ch 0，tag 0x56，头部格式不变）

- bit0 = IDR（现有）
- bit1 = YCoCg（现有，仅 double 有意义）
- **bit2 = single**：1 = 编码帧 W×H BT.601；0 = 2W×H 双打包。layer 基础帧 = single（bit2=1, bit1=0）。

### 2.2 增强层消息（**新增 ch 4**，tag 0x57）

```
[0]    u8   tag = 0x57
[1..8] u64  pts_ms      —— 与同帧基础消息一致，客户端据此配对
[9..]  熵编码残差载荷
```

- `channel_reliable[4]=false`（两端默认即 false）；丢失由客户端按 pts 自然丢弃，**不触发** REQ_KEYFRAME（客户端丢帧回调按 channel 过滤）。
- 残差载荷：`[ver/格式 u8]` + range coder 字节流。残差域 = single I420 平面（Y: W×H，U/V: 各 W/2×H/2，planar 顺序），符号 = `clamp(orig − recon, −128, 127) + 128`；客户端还原 `val = clamp(base + sym − 128)`。
- 尺寸护栏：编码结果 > 512KB → 该帧增强直接弃发。

### 2.3 新命令 0x06 SET_FORMAT（client → phone）

```
[0] u8 = 0x06
[1] u8 mode 位域：
      bit0 几何：0=double(2W×H) 1=single(W×H)
      bit1 色彩（仅 double）：0=raw 1=YCoCg
      bit2 分层：1=layer（叠加 ch 4 增强层）
```

组合：double-ycocg=`0b010`、double-raw=`0b000`、single=`0b001`、**layer-single=`0b101`（本次实现）**、layer-double=`0b110`（后续配置化，协议位已预留）。
server 收后 recreate 编码器 → 新流以 IDR + 新 flags 开始 → 重发 DEVICE_INFO。

### 2.4 向后兼容

- Windows 客户端 `payload[0]!=0x56` 丢弃 → ch4 增强自动忽略。
- 默认模式仍为 double-ycocg，Windows 端无感。

## 3. 共享分层熵编码模块（`shared/layered/`，纯 C++）

server（编码）与 Android 客户端（解码）共用，host 可单测：

- `entropy.h/.cpp`：零段跳过 + 逐平面最优 k Rice 熵编码（无损对照路径，
  协议 §3.4 kind=0x01）。实测体积 ~1.4MB/帧，仅作对照。
- `residual.h/.cpp`：残差计算/还原（`clamp(orig−recon,−128,127)+128`，
  Y+U+V planar；客户端侧交织成 NV12 上传纹理）。
- `camouflage.h/.cpp`：模 256 水平一阶差分/前缀和（方案A 伪装变换）。
  **实验证明不可行**（见 layer_test_report.md），保留作负面对照。
- 线上增强层默认 = 直偏置 sym 帧直接送第二个 MediaCodec 硬编
  （协议 §3.4 kind=0x02），不经本模块熵编码。
- `layered_test.cpp`：host 往返单测，
  `g++ -O2 -std=c++17 shared/layered/entropy.cpp shared/layered/residual.cpp shared/layered/camouflage.cpp shared/layered/layered_test.cpp -o layered_test`

## 4. server 端改造（root 小米）

- **`ScreenEncoder.java`**：
  - mode 位域 + `setFormat(mode)`（synchronized → recreate）。
  - `setup()`：single/layer → `encWidth = W`，打包走 `repack_core.cpp::rgba_to_i420`；double → 2W 现状。
  - layer 模式：基础码率 = min(bitrate, capacity) × base_share；`onEncodedFrame` 把去头 AU 同时喂 BaseDecoder。
  - flags 加 bit2；出站积压丢帧分级：**增强层低阈值先丢**，基础 P 帧维持现有阈值。
- **`BaseDecoder.java`（新增）**：Java MediaCodec 解码器闭环解码基础层（csd-0/1 配置；H.264 解码确定性 → 与客户端重建逐字节一致）；`getOutputImage` 拿 planes + pts 回调；orig packed 帧按 pts 小环形缓存暂存对齐。
- **`Layered.java`（新增 JNI）**：`nativeComputeEnhancement(orig, recon planes…) → byte[]`，native 调 shared/layered。
- **`tight_jni.cpp` / `TightBridge.java`**：新增 `nativeSendChannel(byte[], int ch)`。
- **`ControlInjector.java`**：`case 0x06` → `encoder.setFormat(mode)`。
- **`Server.java`**：新参数 `--mode <double-ycocg|double-raw|single|layer>`（默认不变）、`--base-share 0.35`、`--no-layer`。
- 风险：layer 模式同开硬编+硬解各一路（SD865 可行，`--no-layer` 可退）；增强层天然晚于基础帧 ~10-20ms（基础解码时延），与"赶不上上屏基础帧"语义契合。

## 5. Android 客户端（新增 `client-android/`）

### 5.1 Java 上层（薄）

- `MainActivity`：连接 UI（IP 默认 192.168.43.1 / 端口 / 模式下拉 / 状态）、GLSurfaceView、RECORD_AUDIO 运行时权限、生命周期转发 native。
- `GlRenderer`：GLSurfaceView.Renderer 三回调转发 native（GL 线程 Java 持有，native 只发 GLES2 调用）。
- 输入：`onTouchEvent`→native（action+归一化坐标）；`dispatchKeyEvent`→native（**Android keycode 直通**，无需 vk 映射表）；IME 文本→TEXT 命令。
- `MicCapture`：AudioRecord 16kHz mono 40ms 帧 → native → ch 1（tag 0xA1）。
- `AndroidManifest.xml`：INTERNET + RECORD_AUDIO。

### 5.2 C++ 核心（`client-android/jni/` → libtightcast_client.so）

- `transport.cpp`：tight Leaf（移植 Windows `transport_glue.cpp`；`getpid()` 替代 GetCurrentProcessId；固定 bind 18800 保持 server 内核 NoPorts 规避语义）。
- `decoder.cpp`：AMediaCodec（NDK）+ AImageReader 输出（备选路径）。
  **华为 EMUI 实测 configure 失败（-10000）** → 主路径改为 Java MediaCodec
  buffer 模式（`VideoDec.java`，getOutputImage 平面 + crop 处理 + 惰性
  configure（SPS/PPS 解析自首个 IDR，csd 用 direct buffer，尺寸 mb 对齐）），
  帧经 JNI 回 native 排产（两实例：基础层 + 增强层）。
- `enhancement.cpp`：ch 4 → range 解码 → 残差按 pts 入小 LRU map；pts ≤ 已上屏 pts 的迟到残差丢弃。
- `scheduler.cpp`：移植令牌桶上屏调度 + **配对合成**：帧入队后留 **grace 窗口**
  （默认 150ms，`enhWaitMs` 可调）等增强层赶上；成熟时刻命中同 pts 残差 →
  合成上屏，未命中 → 上屏基础帧；**饿死兜底**：队列无成熟帧且画面停滞
  ≥ grace（新帧断流/首帧未成熟）→ 提前上屏最新基础帧（模糊 base 比冻屏好）。
  grace 是合成命中率的前提（增强层固有滞后 50~150ms，0 等待 = 永远赶不上）。
  迟到残差按 pts ≤ 已上屏 pts 丢弃。
- `renderer.cpp`：GLES2/GLSL ES 三路径 shader（double-raw / double-ycocg / single-BT.601，移植 Windows 公式）+ 残差相加（四纹理，`clamp(base+res−128)` 后再做打包还原）；逐帧按 flags 切路径。
- `client.cpp`：总装（移植 Windows `main.cpp` 门控：got_idr 闸、pts 顺序闸、
  丢帧→REQ_KEYFRAME、关键帧看门狗；**保序全解码**——帧一律提交解码推进参考链，
  上屏丢弃交给 DisplayScheduler，不做"顶帧拒收"（拒收 P 帧会断链引发关键帧风暴））。
- `CMakeLists.txt`：`add_subdirectory(third_party/tight)` + `shared/layered`；链接 `android log GLESv2 mediandk`；arm64-v8a / android-24。

### 5.3 构建（脚本式，仿 server/build.sh，不引 Gradle）

`client-android/build.sh`：NDK cmake → .so；javac → d8；aapt2 compile+link（manifest+最小资源）；zipalign；apksigner（无 keystore 自动生成 debug key）。`run.sh`：adb install + am start。

## 6. 验证（真机）

1. 小米开热点，华为接入；小米 `./server/run.sh`（热点拓扑**不需要** usb-link.sh；热点一般无客户端隔离，不通则 adb reverse 兜底）。
2. 华为装 APK，连 `192.168.43.1:8800`。
3. 验证项：三模式 UI 切换画面格式正确；layer 模式弱网/高负载下"基础帧先上屏、增强赶上则合成"；触控/按键/文字注入；麦克风→小米扬声器；断线重连。
4. 本机可达自动化：`shared/layered` host 单测、`repack_test` 不回归、server javac+d8+NDK 编译、client-android NDK 编译、Windows cmake 构建不受影响。真机运行由用户执行。

## 7. 实施顺序

1. protocol.md 更新 + `shared/layered`（host 单测跑通）
2. server 三模式 + SET_FORMAT + BaseDecoder 闭环 + 增强层发送
3. Android 客户端 JNI 核心（传输→解码→调度→渲染→合成）
4. Java 上层 + APK 构建脚本
5. 文档更新（README、client-android README）+ 真机验证

## 8. 风险与对策

| 风险 | 对策 |
|---|---|
| AImageReader 作解码输出在华为机上兼容性 | Java MediaCodec buffer 模式兜底路径 |
| layer 模式 server 编解双开负载 | SD865 硬编+硬解各一路可行；`--no-layer` 退出 |
| 残差尺寸失控（场景切换） | 512KB 护栏 + 积压先丢增强层 |
| 热点 UDP 不通 | adb reverse / USB 链路兜底 |
| 手机内核 UDP socket connect 首个对端 | 客户端固定 bind 18800（与 Windows 一致） |
