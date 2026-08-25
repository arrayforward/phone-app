# tightcast 协议规范（server ↔ client 共享契约）

基于 `third_party/tight` 可靠 UDP 传输库的手机投屏/远控协议。
**所有多字节整数一律大端（big-endian）。** 两端实现必须严格遵守本文档。

## 1. 链路角色与连接

| 端 | tight 角色 | 说明 |
|---|---|---|
| 手机 server | `LinkRole::Node`，bind `0.0.0.0:8800`（默认端口，可用参数覆盖），id=`"phone"` | 等待接入 |
| PC client | `LinkRole::Leaf`，id=`"pc-<pid>"`（每次运行唯一，防僵尸会话串话），bind `0.0.0.0:18800`（固定端口——实测手机内核会把 server UDP socket "connect" 到首个对端地址，其他来源报文被 NoPorts 丢弃；固定端口后重连来源不变） | `connect()` 到手机 IP:端口 |

默认 token：`"tightcast"`（两端一致，可用命令行参数覆盖）。

两端 `TightConfig` 公共项：

```
mtu                 = 1350        （默认）
encryption_enabled  = true        （默认）
report_interval     = 333ms
late_buffer_ms      = 16
max_message_bytes   = 1 MB        （IDR 帧可能超 64KB 默认值）
retransmit_enabled  = true        （ARQ 机制总开关；关闭会让 channel_reliable 静默失效——
                                  keep_pending 依赖握手通告的 retransmit 能力）
channel_reliable[0] = true        （视频通道 ARQ：大 IDR 分片突发在 USB/RNDIS 上易丢尾包，重传兜底）
channel_reliable[3] = true        （data 通道，设备信息等）
channel_fec_extra[1]= 1           （音频通道额外冗余，仅 client 端需要，两端都设无害）
flush_interval      = 10ms
其余保持默认
```

## 2. 通道分配

| 通道 | 方向 | 内容 | tight API |
|---|---|---|---|
| 0（video） | phone → client | H.264 视频帧 | `send_video(peer, payload, keyframe)` |
| 1（audio） | client → phone | 声音注入 PCM | `send_channel(peer, payload, 1)` |
| command | client → phone | 触控/键盘/滚动/请求关键帧 | `send_command()` |
| command | phone → client | （保留） | `send_command()` |
| 3（data） | phone → client | 设备信息（可靠、去重） | `send_data()` |

两端 `set_message_callback` 收到的数据消息语义固定：server 端收到的
Data 消息 = 音频 PCM（通道 1）；client 端收到的 Data 消息 = 视频帧（通道 0）。

## 3. 视频消息（通道 0，phone → client）

```
[0]    u8   tag = 0x56 ('V')  —— 必须！tight 库的 deliver_message 把载荷首字节
                                0x01/0x02/0x03 保留为内部 file/data 通道路由，
                                IDR 的 flags=0x01 会被误判为 file-manifest 而静默丢弃
[1]    u8   flags        bit0 = 1 表示本帧为 IDR 关键帧
[2..9] u64  pts_ms       编码器输出时间戳（微秒值 / 1000 转毫秒）
[10..] H.264 Annex-B 访问单元（一个完整帧，可含多个 NAL）
```

### 3.1 像素打包：RGB888 → 双 YUV420 左右拼接（"4:4:4 over 4:2:0"）

手机画面是 RGB，而硬编 H.264 只收 4:2:0——直接编码会丢一半色度
（文字/彩边发虚）。为此把每个 RGB888 帧拆成**两个 YUV420 帧**（每帧
1.5 字节/像素，两帧恰好 3 字节/像素 = RGB888），左右拼接成 **2W×H**
的一帧送编码器；client 解码后由 GL shader 逐像素合并还原为 RGB888。

记逻辑帧宽 W、高 H（W、H 均为偶数），像素 (x,y) 的 RGB 为 (R,G,B)，
行序自上而下（top-down，两端一致）：

- 左半帧 A（编码帧 x ∈ [0, W)）、右半帧 B（x ∈ [W, 2W)）
- **亮度平面（全分辨率）**：
  - `Y_A(x, y) = R(x, y)`
  - `Y_B(x, y) = B(x, y)`
- **色度平面（1/4 分辨率，携带全分辨率 G）**：对每个 2×2 块
  (bx,by)，bx ∈ [0, W/2)，by ∈ [0, H/2)：
  - `U_A(bx,by) = G(2bx,   2by  )`   （块内左上像素）
  - `V_A(bx,by) = G(2bx+1, 2by  )`   （右上）
  - `U_B(bx,by) = G(2bx,   2by+1)`   （左下）
  - `V_B(bx,by) = G(2bx+1, 2by+1)`   （右下）

**还原（client shader）**：输出像素 (x,y)，px = x&1，py = y&1：

```
R = Y_A(x, y)
B = Y_B(x, y)
G = (px,py) == (0,0) ? U_A(x/2, y/2)
  : (px,py) == (1,0) ? V_A(x/2, y/2)
  : (px,py) == (0,1) ? U_B(x/2, y/2)
  :                    V_B(x/2, y/2)
```

无任何色彩空间变换（不做 RGB↔YUV 的 BT.601 矩阵）——YUV420 平面只是
容器，直接搬运 R/G/B 字节值，结构性无损（H.264 有损压缩除外）。

### 3.2 YCoCg 打包（v2，默认）——利用通道相关性降失真

§3.1 的原始搬运没利用 RGB 通道相关性。UI 画面大面积近灰（白字深色底），
改用 YCoCg 整数变换后再按同样的双 YUV420 布局打包，色差通道近灰处
平坦、幅度小，编码器几乎不在色度上花比特 → 同码率下还原画质更好。

**前向（server，逐像素整数运算，含偏置）**：
```
Y'  = (R + 2G + B + 2) >> 2        // 真亮度，0..255
Co  = ((R - B) >> 1) + 128         // 橙差，0..255（有界）
Cg  = ((2G - R - B) >> 2) + 128    // 绿差，0..255（有界）
```
**逆变换（client shader）**：
```
cg = Cg - 128; co = Co - 128
G = Y' + cg;  B = Y' - cg - co;  R = Y' - cg + co    // ±1~2 舍入误差
```

**平面布局与 §3.1 完全相同**，仅载荷语义替换：
- `Y_A = Y'`（真亮度，全分辨率）
- `Y_B = Co`（全分辨率，偏置后）
- 4 色度平面按 2×2 块奇偶装 `Cg`（全分辨率，偏置后）

**线上标记**：视频消息头 flags bit1（0x02）= 1 表示本帧为 YCoCg 打包
（bit0 仍是 IDR）。client 按该位选择 shader 还原公式，逐帧自描述，
两模式可混流切换。

- server 采集：虚拟显示 → ImageReader（RGBA_8888）→ JNI native 重排
  （行 stride 需处理）→ 填入 MediaCodec 输入缓冲
  （`COLOR_FormatYUV420Flexible`，`getInputImage()` 按 plane 填充，
  兼容 planar/interleaved 色度）。
- 编码帧尺寸 = 2W×H；SPS 里的画面裁剪由解码器处理，client 拿到的
  NV12 解码帧即 2W×H。
- client：NV12 拆成 Y（2W×H，GL_LUMINANCE）与 UV（W×H/2，
  GL_LUMINANCE_ALPHA，luma=U、alpha=V）两张纹理，fragment shader
  按上面公式合成 RGB888。**DEVICE_INFO 仍携带逻辑尺寸 W×H**（窗口
  纵横比按逻辑尺寸），与编码帧宽度 2W 区分。
- 每个 IDR 帧前必须带上 SPS/PPS（Annex-B）：server 缓存
  `INFO_OUTPUT_FORMAT_CHANGED` 中的 `csd-0`/`csd-1`，拼接到每个 IDR 前面。
- client 在收到首个 IDR 之前不送解码器；丢帧（`message_loss_callback`）
  或画面无法恢复时发送 `REQ_KEYFRAME` 命令。
- server 经 `video_capacity_callback` 调整编码码率；`evac_keyframe_callback`
  / `loan_exhausted_callback` 触发时强制出新 IDR。

## 4. 音频注入消息（通道 1，client → phone）

```
[0]    u8   tag = 0xA1   —— 同样为避免撞上 tight 内部保留首字节（见 §3）
[1..]  PCM s16le，16000 Hz，单声道，640 采样 = 1280 字节（40ms）
```

- 总长 1281B，恰好仍能装入单个 tight 数据报（1286B 载荷），免分片。
- client 用 waveIn 采集麦克风，40ms 一块即发。
- phone 用 `AudioTrack`（`STREAM_MUSIC`，16000Hz，mono，PCM_16BIT）
  收到即写，实现 PC 麦克风声音注入手机扬声器。

## 5. 控制命令（command 通道，client → phone）

首字节为命令类型：

### 0x01 TOUCH
```
[0]    u8  = 0x01
[1]    u8  action    0=DOWN 1=UP 2=MOVE
[2]    u8  slot      触点槽位（v1 仅支持 0，单点触控）
[3..6] f32 x         归一化坐标 0.0~1.0（相对视频画面宽）
[7..10] f32 y        归一化坐标 0.0~1.0（相对视频画面高）
```
server 将归一化坐标乘虚拟显示分辨率得到像素坐标，构造
`MotionEvent`（DOWN/UP/MOVE，source=TOUCHSCREEN，toolType=FINGER），
经 `InputManager.injectInputEvent(ev, INJECT_INPUT_EVENT_MODE_ASYNC)` 注入。

### 0x02 KEY
```
[0]    u8  = 0x02
[1]    u8  action    0=DOWN 1=UP
[2..5] s32 androidKeycode（android.view.KeyEvent 键码，大端）
```
server 构造 `KeyEvent` 注入。HOME/BACK/POWER/音量等系统键走此命令。

### 0x03 TEXT（ASCII 文本输入）
```
[0]    u8  = 0x03
[1..]  UTF-8 文本
```
server 将每个可打印 ASCII 字符映射为（keycode, shift）按键对并依次注入
DOWN+UP；无法映射的字符忽略。client 对 WM_CHAR 可打印字符用此命令，
对控制键/功能键用 0x02。

### 0x04 SCROLL
```
[0]     u8  = 0x04
[1..4]  f32 x   归一化
[5..8]  f32 y   归一化
[9..12] f32 dy  纵向滚动量（正=向上滚/内容下移，约定与 Android AXIS_VSCROLL 一致：正值内容上移即手指上滑）——
                统一约定：dy > 0 表示滚轮向上（屏幕内容向下滚），直接填入 AXIS_VSCROLL。
```
server 在 (x,y) 处注入 `MotionEvent.ACTION_SCROLL`（AXIS_VSCROLL = dy）。

### 0x05 REQ_KEYFRAME
```
[0]    u8  = 0x05
```
server 对编码器 `setParameters(PARAMETER_KEY_REQUEST_SYNC_FRAME)`。
client 在建连后、丢帧回调后各发一次。

## 6. 数据消息（通道 3 data，phone → client，可靠）

### 0x01 DEVICE_INFO
```
[0]     u8  = 0x01
[1..2]  u16 width   视频画面宽（像素）
[3..4]  u16 height  视频画面高（像素）
[5]     u8  name_len
[6..]   UTF-8 设备型号名
```
server 在 client 上线（peer Online）后发送一次；屏幕旋转导致
编码尺寸变化时重发。client 据此设定窗口初始尺寸与纵横比。

## 7. 会话流程

1. server 启动：Node bind 8800，等待。
2. client `connect()` → 握手（X25519 + token）→ Online（含建连测速）。
3. client 发 `REQ_KEYFRAME`。
4. server 创建/确认虚拟显示 + H.264 编码器，开始 `send_video`；
   发 DEVICE_INFO（data 通道）。
5. client 收首 IDR → 初始化解码器 → 渲染；麦克风采集线程开始
   40ms PCM 推送（通道 1）；窗口输入事件 → 命令通道。
6. 任一端掉线：tight 心跳 + dead_timeout 检测，client 自动重连后
   回到第 3 步。

## 8. 低时延约定

- 编码器：`KEY_FRAME_RATE` 默认 30（参数可调到 60）、I 帧间隔 10s
  （靠 REQ_KEYFRAME 恢复，而非频繁 IDR）、`KEY_LATENCY`=0、
  `KEY_PRIORITY`=0（实时优先）。
- client 解码零缓冲：收到一帧立即解码渲染，不做 jitter buffer。
- 触控/键盘走 command 通道（插队、单报文），不与视频排队。
- 音频 40ms 帧由 tight 音频通道绕过令牌桶直发。
