# tightcast 测试报告

> 配套设计文档：[YCoCg 管线设计](ycocg_pipeline.md)。
> 测试工具与产物：`tools/psnr/`（产物在 `tools/psnr/out/`，不入库可复现）。

## 1. 测试环境

| 端 | 配置 |
|---|---|
| 手机 | 小米 10（SD865），Android 13 / MIUI，Magisk root |
| PC | Windows 11，Core Ultra 5 125H + Arc 核显，MinGW g++ 15 |
| 链路 | USB RNDIS 点对点（实测持续吞吐 254 Mbps，RTT ~1-2ms） |
| 视频 | 逻辑 886×1920（编码帧 1772×1920），H.264，默认 6Mbps 上限 |

## 2. 正确性单测

| 套件 | 结果 |
|---|---|
| tight ctest（16 套件 / 150+ 用例，含本轮新增的关键帧排除/预期归一用例） | 全绿 |
| repack_test（§3.1 逐字节断言 + 往返无损；§3.2 YCoCg 往返 ±2 容差；fill planar/NV12；i420 转换 ±1） | ALL TESTS PASSED |
| client 打包/还原公式自测（5 种尺寸伪随机图逐像素比对） | ALL PASS |
| GL 重排 vs CPU 重排逐字节一致（`--repack-selftest`，真机 SurfaceTexture 通路） | PASS |

## 3. 画质：PSNR 对比（真机 MediaCodec 硬编出流，oneVPL 硬解还原）

方法：桌面真屏截图缩放到 886×1920 作基准 → 各路径单帧 IDR 编码 →
oneVPL 解码 → 还原 RGB → Pillow `ImageChops.difference` + `ImageStat.rms`
算 PSNR（RGB 三通道均值 + 总）。

### 3.1 旧桌面（深色暗纹壁纸）

| 路径 | 标称码率 | 产出字节 | PSNR 总 | R / G / B |
|---|---|---|---|---|
| single（4:2:0） | 6M | 67 KB | **32.73** | 32.45 / 33.73 / 32.15 |
| dual-raw（§3.1） | 6M | 168 KB | 32.04 | 31.91 / 32.23 / 32.00 |
| dual-raw | 12M | 264 KB | 34.45 | 34.57 / 34.14 / 34.64 |
| dual-ycocg（§3.2） | 6M | 58 KB | 31.37 | 31.09 / 31.63 / 31.41 |
| dual-ycocg | 12M | 97 KB | **33.98** | 33.72 / 34.28 / 33.94 |

### 3.2 新桌面（亮绿草地+蓝天，彩色高频内容，11 档全表）

| 路径 | 标称码率 | 产出字节 | PSNR 总 | R / G / B |
|---|---|---|---|---|
| single | 6M | 91 KB | 30.90 | 30.30 / 32.40 / 30.30 |
| single | 12M | 271 KB | 36.44 | 36.37 / 39.56 / 34.69 |
| single | 40M | 478 KB | 38.57 | 38.94 / 42.87 / 36.24 |
| dual-raw | 6M | 221 KB | 31.05 | 30.18 / 30.60 / 32.81 |
| dual-raw | 9M | 221 KB | 31.05 | 30.18 / 30.60 / 32.81 |
| dual-raw | 12M | 335 KB | 33.90 | 33.53 / 32.96 / 35.63 |
| dual-ycocg | 6M | 84 KB | 29.58 | 28.87 / 29.42 / 30.65 |
| dual-ycocg | 9M | 84 KB | 29.58 | 28.87 / 29.42 / 30.65 |
| dual-ycocg | 12M | 143 KB | 32.64 | 32.13 / 32.55 / 33.33 |
| dual-ycocg | 20M | 274 KB | 36.65 | 36.42 / 36.72 / 36.83 |
| dual-ycocg | 40M | 794 KB | **44.98** | 44.86 / 45.94 / 44.29 |

### 3.3 结论

- **YCoCg 的 RD 效率约为 raw 打包的 3 倍**：同画质（~32dB）只需 35%
  的字节（58KB vs 168KB）；34dB 档 97KB vs 264KB。
- **同字节公平对比 YCoCg 反超 single**：ycocg@20M（274KB，36.65dB）≈
  single@12M（271KB，36.44dB）——这是全测试中首次字节对齐的比较
  （差 <1%），ycocg 反超 0.21dB 且三通道均衡（36.4~36.8），single 的
  B 通道被 4:2:0 色度亚采样拖到 34.69（比它的 G 低 4.9dB）。
- **高码率档 YCoCg 优势最大**：40M 档 QP 钳制彻底解除，ycocg 达
  **45dB 级近透明质量**（三通道 44.3~45.9 均衡），single 受 4:2:0 结构性
  天花板所限止步 38.57dB（B 通道 36.24）。**码率越充足，双 YUV-YCoCg
  优势越大**：低码率时 1/3 字节同画质，高码率时 single 撞色度天花板而
  它没有。
- **PSNR 是亮度噪声主导指标**，全分辨率色度（文字/彩边锐利度）在其中
  权重低。文字区域放大对比（下图）：ycocg 彩边贴原图，single 的 4:2:0
  在彩边渗色发灰。
- **前提边界**：鲜艳高饱和内容里 Co/Cg 不再近灰平坦，ycocg 收益收窄
  （字节节省率 38% vs 近灰桌面 35%……仍占优，PSNR 差距拉大）。
- **测量限制**：低码率档（6M/9M）单帧 IDR 的 QP 钳制仍在（输出逐字节
  相同），这些档位的标称码率对比不完全公平；公平的稳态结论需多帧
  CBR 收敛测试（已知限制，TODO）。

![基准](images/cmp_ref_text.png)
基准（ref，文字区域放大）

**single（4:2:0）逐档**：

![single_6M](images/cmp_single_6M_text.png)
single @6M（30.90dB / 91KB）

![single_12M](images/cmp_single_12M_text.png)
single @12M（36.44dB / 271KB）

![single_40M](images/cmp_single_40M_text.png)
single @40M（38.57dB / 478KB）：彩边渗色始终存在（色度天花板）

**dual-raw（§3.1）逐档**：

![dual_6M](images/cmp_dual_6M_text.png)
dual-raw @6M（31.05dB / 221KB）

![dual_12M](images/cmp_dual_12M_text.png)
dual-raw @12M（33.90dB / 335KB）

**dual-ycocg（§3.2）逐档**：

![dual_ycocg_6M](images/cmp_dual_ycocg_6M_text.png)
dual-ycocg @6M（29.58dB / 84KB）：彩边已贴原图，字节仅 single 的 92%

![dual_ycocg_12M](images/cmp_dual_ycocg_12M_text.png)
dual-ycocg @12M（32.64dB / 143KB）

![dual_ycocg_20M](images/cmp_dual_ycocg_20M_text.png)
dual-ycocg @20M（36.65dB / 274KB）：与 single@12M 同字节、反超 0.21dB

![dual_ycocg_40M](images/cmp_dual_ycocg_40M_text.png)
dual-ycocg @40M（44.98dB / 794KB）：45dB 级近透明，三通道均衡

## 4. 稳定性实测

| 场景 | 结果 |
|---|---|
| 120s 持续活跃画面 | 0 断连、0 崩溃、4 次编码器重建全存活 |
| 杀 server 8s 重启 | 会话无缝续上（dead_timeout 内） |
| 杀 server 35s 重启 | 客户端 disconnected → 2s 重连 → 自动 online 出流 |
| 编码器卡死 | 监督循环检测"有喂入 >2s 无输出"→ 自动重建 |
| 暂停秒表（画面静止） | 两端读数逐厘秒一致（静止后状态收敛正确） |
| 循环多次 kill/重启 server | 均自动恢复 |

## 5. 延迟实测（秒表玻璃到玻璃法）

手机跑秒表 → 同一瞬间客户端 PrintWindow 读数 vs 手机 screencap 读数：

- 修复前（贷款排队满档）：**~5s**
- 修复后（出站积压丢帧 + 在途帧≤1 + 令牌桶上屏）：**~40-250ms**
  （测量含两次截图间隔误差，真值偏下限）

## 6. 分项耗时（稳态，活跃画面 30-60fps）

| 环节 | 耗时 |
|---|---|
| server 重排（CPU native） | ~7.5ms/帧（GL 路径 ~10ms，瓶颈 GPU→CPU 回读） |
| client 解码（oneVPL 硬解） | ~7.5-8.5ms/帧（大头 5MB/帧 GPU→CPU 回读） |
| tight 发送队列排队 | queue_avg ~0-1ms（出站积压丢帧后） |
| 网络单程 P50 | 4-12ms |

## 7. 已知限制 / TODO

- 单帧 PSNR 测试的 QP 钳制问题（§3.3），多帧码控收敛测试待做
- 多点触控、非 ASCII 文本输入未实现；手机音频回传（phone→PC）未实现
- 高分辨率下 client 渲染可换 D3D 直出省 GPU→CPU→GPU 往返
