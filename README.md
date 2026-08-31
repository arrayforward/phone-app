# tightcast — 基于 tight 的手机投屏与远程操控

在 **root 手机**上运行 server，把屏幕实时投射到 **Windows PC 或另一台 Android 手机**，
并支持远程操控：触控、键盘输入、滚轮，以及 **客户端麦克风声音注入**到手机扬声器。
传输层完全基于本仓库的 `third_party/tight` 可靠 UDP 协议库（加密、FEC、ARQ、
拥塞控制、命令插队通道），链路走 USB RNDIS 点对点（~2ms RTT）或手机热点直连，低时延。

## 文档

| 文档 | 内容 |
|---|---|
| [docs/ycocg_pipeline.md](docs/ycocg_pipeline.md) | **YCoCg 双 YUV420 打包管线设计**（4:4:4 over 4:2:0，配图） |
| [docs/double_ycocg_bandwidth_report.md](docs/double_ycocg_bandwidth_report.md) | **业务汇报：为什么加码率必须加在 double-ycocg 上**（YUV420 vs YCoCg 26 档 RD 对比，面向华为） |
| [docs/test_report.md](docs/test_report.md) | **测试报告**（PSNR 对比 / 稳定性 / 延迟实测） |
| [docs/protocol.md](docs/protocol.md) | 两端线上协议规范（消息格式/通道/tight 配置；含 §3.4 layer 分层） |
| [docs/android_client_plan.md](docs/android_client_plan.md) | **Android 客户端 + 三模式（single/double/layer）方案**（已落地联调） |
| [docs/layered_design.md](docs/layered_design.md) | **分层残差编解码与快速上屏机制详述**（闭环/双参考链/grace P50） |
| [docs/layer_test_report.md](docs/layer_test_report.md) | **分层增强层选型实验**（差分伪装不可行 vs 直偏置硬编可行，配图） |

## 组成

```
phone-app/
├── third_party/tight/   # 可靠 UDP 传输库（投屏/远控的传输层）
├── docs/protocol.md     # 两端共享协议规范（消息格式/通道/tight 配置）
├── shared/layered/      # layer 模式共享模块（残差/Rice 熵编码/差分伪装，host 单测）
├── server/              # Android 端（root 手机，app_process 运行，无需 APK）
│   ├── java/...         # 虚拟显示镜像采集 + MediaCodec H.264 + 输入注入 + AudioTrack
│   │                    #   + layer 模式：BaseDecoder 闭环解码 + EnhEncoder 增强层
│   ├── jni/             # tight 的 JNI 桥 + repack/残差 native（NDK libtight_jni.so）
│   ├── build.sh         # 一键构建（javac + d8 + NDK cmake）
│   ├── run.sh           # adb push + su app_process 前台启动
│   └── usb-link.sh      # 配置 USB RNDIS 点对点链路（重启手机后需重跑）
├── client/              # Windows PC 客户端（C++17，MinGW，零第三方依赖）
│   └── src/             # oneVPL（Intel 硬解）优先 + MF/D3D11 兜底 + Win32/OpenGL 渲染/输入 + waveIn 麦克风
└── client-android/      # Android 手机客户端（APK：Java 上层 + C++ 核心 JNI）
    ├── app/             # Activity/GLSurfaceView/AudioRecord/VideoDec（Java MediaCodec 解码）
    ├── jni/             # tight Leaf + 调度/配对合成/GLES2 shader（libtightcast_client.so）
    ├── build.sh         # 脚本出 APK（javac + d8 + aapt2 + zipalign + apksigner，无 Gradle）
    └── run.sh           # adb install + am start
```

## 视频格式三模式（协议 §3，运行时经 SET_FORMAT 命令切换）

| 模式 | mode 位域 | 说明 |
|---|---|---|
| double-ycocg（默认） | `0b010` | 双 YUV420 YCoCg 打包（4:4:4 over 4:2:0），画质上限 |
| double-raw | `0b000` | 双 YUV420 RGB 原样搬运（对照） |
| single | `0b001` | W×H BT.601 4:2:0 直接编码，编码像素减半 |
| **layer** | `0b101` | **分层**：低码率 single 基础层（ch 0）+ 直偏置残差增强层（ch 4，第二个硬编）；两帧赶上上屏窗口即合成近无损画面，赶不上先上屏低码率帧 |

## 快速开始

前置：手机已 root（Magisk）并开启 USB 调试；PC 与手机通过 USB 连接。

```bash
# 1. 配置 USB RNDIS 链路（手机侧，每次重启手机后执行一次）
./server/usb-link.sh            # 多设备时：./server/usb-link.sh <serial>

# 2. 构建并部署手机端 server
./server/build.sh
./server/run.sh                 # 前台运行；也可按 run.sh 里的命令后台启动

# 3. 构建并启动 PC 客户端
cmake -B build -S . -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=g++
cmake --build build -j
./build/client/tightcast-client.exe 169.254.0.1 --port 8800
```

`169.254.0.1` 是 usb-link.sh 给手机 rndis0 配置的 link-local 地址；PC 侧
RNDIS 网卡会自动获得 169.254.x.x 地址，无需管理员改 IP。同一可信 LAN 内
也可以直接连手机 WiFi IP（注意很多 AP 阻断客户端间 UDP）。

### Android 手机客户端（第二台手机做显示端）

```bash
./client-android/build.sh
./client-android/run.sh                 # 或 adb install -r client-android/out/tightcast-client.apk
```

- 界面输入 server IP/端口（手机热点拓扑下默认 `192.168.43.1:8800`），模式下拉
  可切换 single/double/layer（需已连接）。
- 触控 = 远程触控；物理按键直通注入（BACK 留给本机）；屏幕按钮发 返回/Home/最近。
- 麦克风注入默认开启（RECORD_AUDIO 权限），状态栏显示上屏统计
  （shown/composited/enh_idr + 双解码器状态）。
- adb 自动化：`am start -n com.tightcast.client/.MainActivity --es host <ip>
  --ei port 8800 --ez autoconnect true [--ei mode 5] [--ei enhWaitMs 150]`

#### 真机验证拓扑（已实测）

小米 10（root）开热点（`cmd wifi start-softap <ssid> wpa2 <pass>` 或设置里开），
华为接入热点，客户端连 server 的热点 IP。layer 模式：
`./server/run.sh --mode layer`（热点拓扑不需要 usb-link.sh）。
注意：企业 WiFi 常见客户端隔离（ping 不通即为此情况），必须走热点或 USB RNDIS。

## 客户端用法（Windows）

```
tightcast-client <phone_ip> [--port 8800] [--token tightcast] [--no-audio]
```

- 鼠标：点击/拖拽 = 触控；滚轮 = 滚动
- 键盘：可打印字符直接输入；Esc=返回，F1=Home，F2=菜单，F3=最近任务，
  F4=电源；方向键/删除/回车等按 Android keycode 注入
- 声音注入：默认开启，PC 麦克风 → 手机扬声器（16kHz mono PCM 40ms 帧），
  `--no-audio` 关闭

完整键位映射见 `client/src/input_map.cpp` 与 `client/README.md`。

## server 参数

```
--port 8800  --token tightcast  --bitrate 6000000  --fps 30  --max-size 1920
--mode double-ycocg|double-raw|single|layer   # 视频格式模式（默认 double-ycocg）
--base-share 0.35                             # layer 模式基础层码率占比
--enh-impl h264|rice                          # layer 增强层实现（默认 h264 直偏置硬编）
--no-audio  --gl-repack  --yuv-raw（=double-raw）
```

## 工作原理（要点）

- **画面（RGB 4:4:4 over 4:2:0）**：`SurfaceControl.createDisplay` 创建虚拟显示
  镜像主屏 layerStack → ImageReader 取 RGBA_8888 → 按 protocol §3.1 把 RGB888
  拆成左右拼接的双 YUV420（2W×H：左半帧 Y=R、右半帧 Y=B、色度平面按 2×2 块
  奇偶装全分辨率 G）→ MediaCodec buffer 输入编码 → tight 视频通道（ch 0）。
  色度零亚采样，文字/彩边锐利度远好于直接 4:2:0 编码。重排默认 CPU（~7.5ms/帧），
  `--gl-repack` 可选 GPU shader 重排（省 CPU 但 GPU→CPU 回读使其更慢 ~10ms）。
  client 解码后由 GL shader 逐像素合并还原 RGB888（无 CPU 色彩转换）。
  静态画面不重复编码（SurfaceFlinger 只在内容变化时合成）。部分编码器
  （如 MIUI/小米）忽略同步帧请求，server 侧在请求关键帧超时未出 IDR 时整体
  重建编码器。
- **操控**：鼠标/键盘事件 → tight command 通道（单报文、插队、保序）→
  `InputManager.injectInputEvent` 注入（root/shell 权限）。
- **声音注入**：PC waveIn 采集 16kHz mono PCM，40ms 一帧走 tight 音频通道
  （ch 1，绕过令牌桶），手机 AudioTrack 即收即播。
- **码率自适应**：tight 的 `video_capacity_bps` 回调直接驱动编码器码率；
  拥塞排空/贷款耗尽回调触发新 IDR。

## 关键坑（已解决，详见 git 历史与代码注释）

1. **企业 WiFi 阻断客户端间 UDP** → 走 USB RNDIS；Android 策略路由会把
   本机回包导去 wlan0，需补 `ip route ... table wlan0`（usb-link.sh 已处理）。
2. **tight 载荷首字节 0x01/0x02/0x03 被库内部保留**（file/data 路由）——
   视频/音频消息必须带 tag 前缀（0x56/0xA1），否则 IDR（首字节恰为 0x01）
   会被静默丢弃。
3. **MF H.264 decoder 的 AVC1 子类型期望 AVCC 码流**——Annex-B 输入必须用
   `MFVideoFormat_H264` 子类型，否则永远无输出。
4. **MIUI 编码器不出 IDR**（忽略同步帧请求且不打 KEY_FRAME 标志）——
   bitstream 扫描 NAL type=5 兜底 + 超时重建编码器。
5. **client 固定端口 18800**：手机内核会把 server UDP socket "connect" 到
   首个对端，后续其他来源的报文被 NoPorts 丢弃；固定端口 + server 按
   `peers()` 动态选择发送目标 + 掉线后 server 主动退出由监督循环拉起
   （run.sh 的 while true）解决。client 侧 start 失败（端口释放延迟）自动
   重试 20 次。
6. **锁序**：JNI 层 g_mutex 只能取快照，持它进 tight 内部锁会与"tight 线程
   持内部锁回调 Java→再取 g_mutex"形成 ABBA 死锁（曾表现为假 30s 断连）。
7. **关键帧不污染带宽估算**（tight 库改动）：send_video 的 keyframe 标志穿透
   到线上 reserved bit11，接收端重组器对关键帧报文跳过报文级延迟直方图/迟到
   统计——大 IDR 突发的串行化延迟是帧自身传输，不再误触发 delay-based 降速。
   帧级迟到判定（F/btl 折算）原已豁免。
8. **MediaCodec 异步回调**（消除 10ms 轮询等待）：app_process 线程无 Looper，
   configure 前须 `Looper.prepare()`，且 setCallback 须在 configure 之后、
   显式挂 HandlerThread；编码器在途帧限 1（防流水线排队积压）。
9. **D8 对匿名类/非静态内部类会内部 NPE**——回调类一律 static 具名嵌套 +
   owner 引用（见 CodecCallback/GlSink）。

## 已知限制

- 单点触控（无多点/手势缩放）；非 ASCII 文本输入未实现
- 声音注入为 PC→手机单向（无手机音频回传）
- 渲染用 OpenGL（WGL 纹理四边形，GPU 缩放；如需更低 CPU 占用可再换 D3D/硬解直出）
- 受 Android 安全限制，银行/DRM 等 secure 页面无法采集
