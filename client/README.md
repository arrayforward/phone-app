# tightcast-client（Windows PC 客户端）

手机投屏/远控的 PC 端：接收 Android server 经 tight（可靠 UDP）推送的 H.264
画面，Media Foundation 解码、Win32 窗口渲染；鼠标/键盘事件转为控制命令注入
手机；本机麦克风 PCM 推给手机播放（声音注入）。协议见 `docs/protocol.md`。

## 构建

```sh
cmake -B build -S . -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=g++
cmake --build build -j
# 产物：build/client/tightcast-client.exe
```

依赖：MinGW g++（C++17）、Windows SDK 自带的 Media Foundation / WinMM
导入库（`mfplat mf mfuuid ole32 winmm`），无第三方依赖。

## 使用

```
tightcast-client <phone_ip> [--port 8800] [--token tightcast] [--no-audio]
       [--max-queue 2] [--display-fps 30]
```

- 连上后自动请求关键帧，收到画面即解码显示；丢帧自动请求新关键帧。
- 掉线每 2s 自动重连。
- `--no-audio` 关闭麦克风采集（默认开启，waveInOpen 失败仅告警不影响视频）。
- 显示调度（令牌桶上屏，防解码端无限堆积）：每 1/display_fps 发一个令牌，
  解码帧有令牌立即上屏、无令牌按 `--max-queue n` 冗余排队、**队满丢最旧留最新**
  （最新帧必须最终上屏——画面静止后不再来新帧，丢最新帧端侧会永远停在旧状态）；
  卡顿期令牌积攒（欠帧），恢复后可补屏。`stderr` 的 `[stats]` 行有
  `disp_drop`（调度丢帧数）可观察。

## 键盘映射约定

可打印 ASCII（字母、数字、空格、符号）经 WM_CHAR → TEXT(0x03) 发送，
不在 KEY(0x02) 映射内，避免双重注入。VK → Android keycode 映射表见
`src/input_map.cpp`（`kVkMap[]`），要点：

| PC 键 | Android |
|---|---|
| 方向键 | DPAD_UP/DOWN/LEFT/RIGHT |
| Enter / Backspace / Tab / Delete | ENTER / DEL / TAB / FORWARD_DEL |
| Home / End / PgUp / PgDn | MOVE_HOME / MOVE_END / PAGE_UP / PAGE_DOWN |
| Ctrl / Shift / Alt（左右） | CTRL_* / SHIFT_* / ALT_* |
| **Esc** | **KEYCODE_BACK（手机返回）** |
| **F1** | **KEYCODE_HOME（回桌面）** |
| **F2** | **KEYCODE_MENU** |
| **F3** | **KEYCODE_APP_SWITCH（最近任务）** |
| **F4** | **KEYCODE_POWER（电源/锁屏）** |
| F5–F12 | KEYCODE_F5–F12 |
| 音量+/-/静音 | VOLUME_UP / VOLUME_DOWN / VOLUME_MUTE |

鼠标：左键按下/拖动/抬起 = 单点触控 DOWN/MOVE/UP（MOVE 8ms 节流），
滚轮 = SCROLL（dy>0 表示滚轮向上），坐标均按视频显示区域归一化。

## 模块

- `src/main.cpp` — 命令行、模块串接、GUI 消息循环
- `src/transport_glue.*` — tight Leaf 配置（protocol 第 1 节）、重连、REQ_KEYFRAME
- `src/decoder.*` — MF H.264 解码（优先 D3D11 硬件加速，软解兜底；RGB32/NV12/I420 → BGRA）
- `src/renderer.*` — Win32 窗口、OpenGL（WGL）纹理渲染、输入事件
- `src/audio.*` — waveIn 16kHz mono 40ms 采集
- `src/input_map.*` — VK → Android 键码表
- `src/commands.h` — 控制命令字节流构造（大端）
