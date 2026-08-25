package com.tightcast.server;

import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.media.Image;
import android.media.ImageReader;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.view.Surface;

import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.ArrayBlockingQueue;

/**
 * 虚拟显示镜像主屏 + MediaCodec H.264 编码（参考 scrcpy 做法）。
 *
 * 采集路径（协议 §3.1 "4:4:4 over 4:2:0"）：虚拟显示（反射 SurfaceControl 隐藏 API
 * 创建，secure=false，投影主屏 layerStack）的内容重排为左右拼接的双 YUV420
 * （2W×H I420：左半帧 Y=R、右半帧 Y=B、色度平面按 2×2 块奇偶装 G）。两条重排路径：
 * 默认 CPU（target = ImageReader RGBA_8888，native Repack/repack_core 重排，
 * 实测 ~7.5ms/帧）；--gl-repack 走 GPU（target = SurfaceTexture，gather shader 打包
 * + glReadPixels 回读，省 CPU 但 ~10ms/帧——瓶颈是 GPU→CPU 回读带宽）。
 * GL 初始化失败自动回退 CPU。两路径输出逐字节一致（--repack-selftest 验证）。
 * 编码线程把 I420 按 plane 填入 MediaCodec（COLOR_FormatYUV420Flexible，2W×H，
 * buffer 输入）。帧率由屏幕内容变化自然驱动（SurfaceFlinger 有更新才回调）。
 *
 * 输出按协议第 3 节组包（tag 0x56 + flags u8 + pts_ms u64 BE + Annex-B AU，
 * IDR 前拼 SPS/PPS）经 TightBridge.nativeSendVideo 发出。
 */
public final class ScreenEncoder {

    private static final String MIME = "video/avc";
    private static final int I_FRAME_INTERVAL_S = 10;
    private static final long ROTATION_CHECK_INTERVAL_MS = 1000;
    private static final int REPACK_STATS_INTERVAL = 100;

    private final int bitrate;
    private final int fps;
    private final int maxSize;
    private final boolean glRepackEnabled;
    // YCoCg 打包（协议 §3.2，默认开）：视频消息头 flags bit1 标记
    private final boolean ycocgEnabled;

    private volatile boolean running;
    private Thread thread;
    private volatile Runnable onDisplaySizeChanged;

    /** 旋转/尺寸变化导致重建后回调（编码线程），用于重发 DEVICE_INFO。 */
    public void setOnDisplaySizeChanged(Runnable callback) {
        this.onDisplaySizeChanged = callback;
    }

    // 编码线程独占访问（codec 另被 requestKeyframe/setBitrate 在 synchronized 下触碰）
    private MediaCodec codec;
    private IBinder display;
    private byte[] csd0; // SPS (Annex-B)
    private byte[] csd1; // PPS (Annex-B)
    private int videoWidth;    // 逻辑画面宽 W（DEVICE_INFO 报这个）
    private int videoHeight;   // 逻辑画面高 H
    private int encWidth;      // 编码帧宽 2W（双 YUV420 左右拼接）
    private int encHeight;     // 编码帧高 H
    private int frameBytes;    // 2W*H*3/2
    private int deviceRotation;
    private int deviceWidth;
    private int deviceHeight;
    private long lastRotationCheck;
    private int currentBitrate;
    // 关键帧请求（任意线程置位，编码线程消费）与最近一次 IDR 时间
    private volatile boolean keyframeReq;
    private long lastIdrAt;
    // 编码器卡死自愈（监督线程）：有喂入但 >2s 无输出 → 编码器驱动断开/
    // 死锁，自动重建，客户端不会卡在旧画面
    private long lastOutputAt;
    private long lastFedSeen;
    private long lastFedChangeAt;
    private static final long IDR_RECREATE_MIN_MS = 2500;

    // ---- 采集：重排线程 → pending 槽；编码线程喂帧 ----
    private HandlerThread imageThread;
    private Handler imageHandler;
    private HandlerThread codecThread;   // MediaCodec 回调线程
    private Handler codecHandler;
    private ImageReader imageReader;   // CPU 兜底路径
    private GlRepack glRepack;         // GL 重排路径（--gl-repack 可选；默认 CPU 更快）
    // 重排输出缓冲 ping-pong：空闲两块，重排线程取一块填充后挂到 pending 槽
    private final ArrayBlockingQueue<ByteBuffer> freeBufs = new ArrayBlockingQueue<>(2);
    // 采集/重建互斥：onImageAvailable 持锁重排（~9ms），teardown 持锁关 reader——
    // 否则重建期 close 掉 reader 会释放其已 acquire 图像的 gralloc 缓冲，
    // 重排线程读野指针 SIGSEGV（tombstone 实录：repack+100 on image-reader）
    private final Object captureLock = new Object();
    private final Object frameLock = new Object();
    private ByteBuffer pendingFrame;   // frameLock 守护；最新帧覆盖旧帧（丢旧保新）
    private long pendingPtsUs;
    // 重排耗时统计（重排线程写，打日志用；无须同步，近似值即可）
    private long repackNanosTotal;
    private long repackCount;
    private long fedFrames;
    private long droppedFrames;
    private long outFrames;   // 编码器累计输出帧（管道时延仪器用）
    private boolean inputCbLogged;   // 诊断：首个输入缓冲回调
    // 编码器在途帧数（queueInputBuffer − dequeueOutputBuffer）：只允许 ≤1，
    // 防编码器内部流水线排队积压（帧龄实测曾达 42ms 的主要贡献项）
    private int inFlight;   // 编码线程独占
    // 帧龄统计（编码线程写）：编码完成时刻 − SurfaceFlinger 合成时刻
    private long sendAgeMsTotal;
    private long sendAgeCount;

    /** GL 重排的帧出口：与 CPU 路径共用 freeBufs / pending 槽。 */
    private final GlRepack.Sink glSink = new GlSink(this);

    private static final class GlSink implements GlRepack.Sink {
        private final ScreenEncoder owner;

        GlSink(ScreenEncoder owner) {
            this.owner = owner;
        }

        @Override
        public ByteBuffer acquireBuffer() {
            return owner.freeBufs.poll();
        }

        @Override
        public void onFrame(ByteBuffer buf, long ptsUs) {
            owner.offerPending(buf, ptsUs);
        }
    }

    public ScreenEncoder(int bitrate, int fps, int maxSize) {
        this(bitrate, fps, maxSize, true);
    }

    public ScreenEncoder(int bitrate, int fps, int maxSize, boolean glRepackEnabled) {
        this(bitrate, fps, maxSize, glRepackEnabled, true);
    }

    public ScreenEncoder(int bitrate, int fps, int maxSize, boolean glRepackEnabled,
                         boolean ycocgEnabled) {
        this.bitrate = bitrate;
        this.fps = fps;
        this.maxSize = maxSize;
        this.glRepackEnabled = glRepackEnabled;
        this.ycocgEnabled = ycocgEnabled;
    }

    public int getVideoWidth() {
        return videoWidth;
    }

    public int getVideoHeight() {
        return videoHeight;
    }

    public void start() {
        if (running) return;
        running = true;
        imageThread = new HandlerThread("image-reader");
        imageThread.start();
        imageHandler = new Handler(imageThread.getLooper());
        // app_process 无线程 Looper：MediaCodec 回调必须显式挂 HandlerThread，
        // 否则 setCallback(cb, null) 静默失效（onInputBufferAvailable 永不触发）
        codecThread = new HandlerThread("codec-cb");
        codecThread.start();
        codecHandler = new Handler(codecThread.getLooper());
        thread = new Thread(this::encodeLoop, "screen-encoder");
        thread.start();
    }

    public void stop() {
        running = false;
        if (thread != null) {
            try {
                thread.join(2000);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
        }
        if (imageThread != null) {
            imageThread.quitSafely();
            imageThread = null;
            imageHandler = null;
        }
        if (codecThread != null) {
            codecThread.quitSafely();
            codecThread = null;
            codecHandler = null;
        }
    }

    /** REQ_KEYFRAME：请求编码器立即出一个 IDR。任意线程可调用。 */
    public synchronized void requestKeyframe() {
        // 标记给编码线程：先试 setParameters 请求同步帧（便宜的设备上有效）；
        // 若超过 IDR 最长期望间隔仍未出 IDR（部分编码器忽略同步帧请求，
        // 且静态画面下 i-frame-interval 也不产出），编码线程整体重建编码器
        // （新编码器首帧必为 IDR + 新 CSD）。
        keyframeReq = true;
        MediaCodec c = codec;
        if (c == null) return;
        try {
            Bundle b = new Bundle();
            b.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
            c.setParameters(b);
            System.out.println("[ScreenEncoder] sync frame requested");
        } catch (Exception e) {
            System.out.println("[ScreenEncoder] request sync frame failed: " + e);
        }
    }

    /** video_capacity_callback → 调整编码码率；变化 >15% 才下发。任意线程可调用。
     *  --bitrate 是硬上限：只能降不能超（链路好也不加码，码率预算是应用语义）。 */
    public synchronized void setBitrate(long bps) {
        MediaCodec c = codec;
        if (c == null || bps <= 0) return;
        if (bps > bitrate) bps = bitrate;  // 硬上限 = 配置值
        long cur = currentBitrate;
        if (Math.abs(bps - cur) * 100 <= cur * 15) return;
        try {
            Bundle b = new Bundle();
            b.putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, (int) Math.min(bps, Integer.MAX_VALUE));
            c.setParameters(b);
            currentBitrate = (int) bps;
            System.out.println("[ScreenEncoder] bitrate -> " + bps);
        } catch (Exception e) {
            System.out.println("[ScreenEncoder] set bitrate failed (ignored): " + e);
        }
    }

    /** ImageReader 回调（image-reader 线程）：拿最新帧 → native 重排 → 挂 pending 槽。 */
    private void onImageAvailable(ImageReader reader) {
        Image img = null;
        try {
            // 重建期间旧 reader 的滞留回调直接丢弃（尺寸可能已变）；
            // 与 teardown 互斥：防重排途中 reader 被 close、gralloc 释放
            synchronized (captureLock) {
                if (reader != imageReader) return;
                img = reader.acquireLatestImage();
                if (img == null) return;
                // 尺寸竞态防御：recreate/旋转期间 image 的实际尺寸可能与当前
                // videoWidth/Height 不一致（旧 reader 滞留帧、窗口过渡），
                // 不符即丢帧——native 重排按 (w,h) 读源，尺寸错会越界崩溃
                if (img.getWidth() != videoWidth || img.getHeight() != videoHeight) return;
                ByteBuffer dst = freeBufs.poll();
                if (dst == null) return; // 编码侧消费不过来，丢帧保低时延
                if (dst.capacity() < videoWidth * 2 * videoHeight * 3 / 2) {
                    freeBufs.offer(dst);
                    return;  // 缓冲是旧尺寸的，丢帧等重建分配新缓冲
                }
                long t0 = System.nanoTime();
                Repack.repack(img, videoWidth, videoHeight, dst, ycocgEnabled);
                long dt = System.nanoTime() - t0;
                // 帧出生时刻用 SurfaceFlinger 的合成时间戳（nanoTime 基准），
                // 端到端时延分析用：onEncodedFrame 里 now-pts = 采集→编码完成时延
                long ptsUs = img.getTimestamp() / 1000;

                repackNanosTotal += dt;
                repackCount++;
                if (repackCount % REPACK_STATS_INTERVAL == 0) {
                    System.out.println("[ScreenEncoder] cpu repack avg "
                            + (repackNanosTotal / repackCount / 1000) / 1000.0 + "ms over "
                            + repackCount + " frames, fed=" + fedFrames + " dropped=" + droppedFrames);
                }
                offerPending(dst, ptsUs);
            }
        } catch (Exception e) {
            System.out.println("[ScreenEncoder] onImageAvailable error: " + e);
        } finally {
            if (img != null) img.close();
        }
    }

    /** 重排完成的帧挂到 pending 槽（新帧覆盖旧帧，丢旧保新）。GL/CPU 两条路径共用。 */
    private void offerPending(ByteBuffer dst, long ptsUs) {
        synchronized (frameLock) {
            ByteBuffer old = pendingFrame;
            pendingFrame = dst;
            pendingPtsUs = ptsUs;
            if (old != null) freeBufs.offer(old); // 旧 pending 未被编码即被覆盖：回收
        }
        tryFeed();  // 新帧到达即尝试喂编码器（异步模式无轮询）
    }

    // ---- MediaCodec 异步回调：消除 10ms 轮询等待（capture→encoded 时延大头）----

    // 编码器空闲输入缓冲下标队列（frameLock 守护）
    private final java.util.ArrayDeque<Integer> freeInputs = new java.util.ArrayDeque<>();

    // 注意：必须是 static 具名嵌套类——D8 对匿名类/非静态内部类有内部 NPE
    // bug（GlSink 同此处理），故持 owner 引用访问外层成员。
    private static final class CodecCallback extends MediaCodec.Callback {
        private final ScreenEncoder owner;

        CodecCallback(ScreenEncoder owner) {
            this.owner = owner;
        }

        @Override
        public void onInputBufferAvailable(MediaCodec c, int index) {
            synchronized (owner.frameLock) {
                if (c != owner.codec) return;   // recreate 后旧编码器的滞留回调丢弃
                owner.freeInputs.add(index);
                if (!owner.inputCbLogged) {
                    owner.inputCbLogged = true;
                    System.out.println("[ScreenEncoder] first input buffer available, idx=" + index);
                }
            }
            owner.tryFeed();
        }

        @Override
        public void onOutputBufferAvailable(MediaCodec c, int index,
                                            MediaCodec.BufferInfo info) {
            synchronized (owner.frameLock) {
                if (owner.inFlight > 0) owner.inFlight--;
            }
            try {
                synchronized (owner) {  // 与 recreate/teardown 互斥
                    if (!owner.running || c != owner.codec) return;
                    if (index >= 0) {
                        owner.onEncodedFrame(c, index, info);
                        c.releaseOutputBuffer(index, false);
                    }
                }
            } catch (Exception e) {
                System.out.println("[ScreenEncoder] onOutputBufferAvailable error: " + e);
            }
            owner.tryFeed();  // 在途减一，尝试喂下一帧
        }

        @Override
        public void onError(MediaCodec c, MediaCodec.CodecException e) {
            System.out.println("[ScreenEncoder] codec error: " + e);
        }

        @Override
        public void onOutputFormatChanged(MediaCodec c, MediaFormat format) {
            synchronized (owner) {
                if (c == owner.codec) owner.cacheConfigData(format);
            }
        }
    }

    private final CodecCallback codecCallback = new CodecCallback(this);

    /** pending 槽有新帧或有空闲输入缓冲时调用：条件满足就把最新帧喂给编码器。 */
    private void tryFeed() {
        ByteBuffer frame;
        long ptsUs;
        int index;
        MediaCodec c;
        synchronized (frameLock) {
            c = codec;
            if (!running || c == null || inFlight > 0) return;
            if (pendingFrame == null || freeInputs.isEmpty()) return;
            frame = pendingFrame;
            ptsUs = pendingPtsUs;
            pendingFrame = null;
            index = freeInputs.poll();
            inFlight++;
        }
        synchronized (this) {  // 与 recreate/teardown 互斥
            try {
                Image input = c.getInputImage(index);
                if (input != null) {
                    Repack.fillInputImage(frame, encWidth, encHeight, input);
                    c.queueInputBuffer(index, 0, frameBytes, ptsUs, 0);
                    fedFrames++;
                } else {
                    droppedFrames++;
                    synchronized (frameLock) { if (inFlight > 0) inFlight--; }
                }
            } catch (Exception e) {
                System.out.println("[ScreenEncoder] feed error: " + e);
                synchronized (frameLock) { if (inFlight > 0) inFlight--; }
            }
        }
        freeBufs.offer(frame);
    }

    /** 监督线程：只保留低频职责（旋转轮询、关键帧重建兜底），收发全走回调。 */
    private void encodeLoop() {
        // app_process 线程无 Looper：MediaCodec.configure() 需要调用线程的 Looper
        // 来创建内部 EventHandler，否则 setCallback 时 NPE（实测 MIUI Android 13）
        android.os.Looper.prepare();
        try {
            setup();
        } catch (Exception e) {
            System.out.println("[ScreenEncoder] setup failed: " + e);
            e.printStackTrace(System.out);
            running = false;
            return;
        }
        System.out.println("[ScreenEncoder] encoding " + encWidth + "x" + encHeight
                + " (logical " + videoWidth + "x" + videoHeight + ") @" + fps + "fps "
                + currentBitrate + "bps");

        while (running) {
            try {
                Thread.sleep(200);
                // 1s 轮询主屏旋转/尺寸变化，变化时重建虚拟显示 + 编码器
                long now = System.currentTimeMillis();
                if (now - lastRotationCheck >= ROTATION_CHECK_INTERVAL_MS) {
                    lastRotationCheck = now;
                    if (displayChanged()) {
                        System.out.println("[ScreenEncoder] display changed, recreating");
                        recreate();
                    }
                }
                // 关键帧请求超时未出 IDR（编码器忽略同步帧请求/静态画面不产出）
                // → 整体重建编码器，新编码器首帧必为 IDR
                if (keyframeReq) {
                    keyframeReq = false;
                    if (now - lastIdrAt > IDR_RECREATE_MIN_MS) {
                        System.out.println("[ScreenEncoder] no IDR for "
                                + (now - lastIdrAt) + "ms, recreating encoder");
                        recreate();
                    }
                }
                // 编码器卡死自愈：有喂入但 >2s 无输出 → 重建
                if (fedFrames != lastFedSeen) {
                    lastFedSeen = fedFrames;
                    lastFedChangeAt = now;
                }
                if (lastFedChangeAt > 0 && now - lastFedChangeAt < 5000
                        && lastOutputAt > 0 && now - lastOutputAt > 2000) {
                    System.out.println("[ScreenEncoder] encoder stalled (fed but no output >2s), recreating");
                    recreate();
                    lastFedChangeAt = 0;
                }
            } catch (InterruptedException ignored) {
            } catch (Exception e) {
                System.out.println("[ScreenEncoder] supervise error: " + e);
                e.printStackTrace(System.out);
                break;
            }
        }
        teardown();
        System.out.println("[ScreenEncoder] stopped");
    }

    private void onEncodedFrame(MediaCodec c, int index, MediaCodec.BufferInfo info) {
        if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
            return; // csd 已由 onOutputFormatChanged 缓存
        }
        if (info.size == 0) return;
        ByteBuffer buf = c.getOutputBuffer(index);
        if (buf == null) return;
        boolean isIdr = (info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;
        long ptsMs = info.presentationTimeUs / 1000;
        lastOutputAt = System.currentTimeMillis();  // 编码器卡死自愈的输出心跳
        // 帧龄统计：编码完成时刻 − 帧出生（SurfaceFlinger 合成）时刻
        if (info.presentationTimeUs > 0) {
            sendAgeMsTotal += System.nanoTime() / 1000 - info.presentationTimeUs;
            sendAgeCount++;
            if (sendAgeCount % REPACK_STATS_INTERVAL == 0) {
                System.out.println("[ScreenEncoder] capture→encoded avg "
                        + (sendAgeMsTotal / sendAgeCount / 1000) + "ms over " + sendAgeCount + " frames");
            }
        }

        ByteBuffer dup = buf.duplicate();
        dup.position(info.offset);
        dup.limit(info.offset + info.size);
        if (!isIdr && containsIdr(dup, info.size)) {
            // 部分编码器（MIUI/小米）不在 BufferInfo 里打 KEY_FRAME 标志，
            // 直接扫 Annex-B NAL type=5 兜底，否则播放端永远等不到 IDR
            isIdr = true;
        }
        if (isIdr) {
            lastIdrAt = System.currentTimeMillis();
            System.out.println("[ScreenEncoder] IDR frame " + info.size + "B (flag="
                    + ((info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0) + ")");
        }

        int head = 10;  // tag(1) + flags(1) + pts(8)
        int csdLen = (isIdr && csd0 != null && csd1 != null) ? csd0.length + csd1.length : 0;
        byte[] packet = new byte[head + csdLen + info.size];
        ByteBuffer out = ByteBuffer.wrap(packet).order(ByteOrder.BIG_ENDIAN);
        out.put((byte) 0x56);  // 'V'：避开 tight 内部保留首字节 0x01/0x02/0x03
        // flags：bit0=IDR；bit1=YCoCg 打包（协议 §3.2，逐帧自描述）
        out.put((byte) ((isIdr ? 1 : 0) | (ycocgEnabled ? 0x02 : 0)));
        out.putLong(ptsMs);
        if (csdLen > 0) {
            out.put(csd0);
            out.put(csd1);
        }
        buf.position(info.offset);
        buf.limit(info.offset + info.size);
        out.put(buf);

        // 出站积压丢帧（低时延铁律）：tight 贷款允许队列积压到 5s 债——
        // 码率略超 btl 时每帧排队可达秒级（实测 ~5s 端到端延迟）。发送前查
        // 出站队列，积压超阈值（~100ms 数据量）的帧直接不进队：P 帧不送 →
        // 对端缺口 → 客户端丢帧回调请求关键帧恢复。IDR 不丢（参考链种子）。
        if (!isIdr) {
            long[] s = TightBridge.nativeStats();
            if (s != null && s[1] > 0) {
                long queueLimitBytes = Math.max(64L * 1300L, s[1] / 10);  // ~100ms 积压
                long queueBytes = s[3] * 1300L;
                if (queueBytes > queueLimitBytes) {
                    droppedFrames++;
                    return;
                }
            }
        }
        TightBridge.nativeSendVideo(packet, isIdr);

        // 编码器内部排队时延仪器（每 100 帧打一次）：pts 是重排完成时刻
        // （queueInputBuffer 时写入），与输出时刻之差 = 帧在 MediaCodec
        // 内部的驻留时间——定位端到端延迟里编码器管道的占比
        if (outFrames % 100 == 0) {
            long queueMs = (System.nanoTime() / 1000 - info.presentationTimeUs) / 1000;
            System.out.println("[ScreenEncoder] codec pipeline delay " + queueMs
                    + "ms (out=" + outFrames + ")");
        }
        outFrames++;
    }

    private void cacheConfigData(MediaFormat format) {
        ByteBuffer csd0Buf = format.getByteBuffer("csd-0");
        ByteBuffer csd1Buf = format.getByteBuffer("csd-1");
        if (csd0Buf != null && csd1Buf != null) {
            csd0 = toArray(csd0Buf);
            csd1 = toArray(csd1Buf);
            System.out.println("[ScreenEncoder] cached csd-0=" + csd0.length + "B csd-1="
                    + csd1.length + "B");
        }
    }

    private static byte[] toArray(ByteBuffer buf) {
        ByteBuffer dup = buf.duplicate();
        byte[] arr = new byte[dup.remaining()];
        dup.get(arr);
        return arr;
    }

    /** 扫描 Annex-B 字节流是否包含 IDR NAL（type 5）。从当前 position 相对读取。 */
    private static boolean containsIdr(ByteBuffer buf, int size) {
        int run = 0; // 连续 0 字节计数（起始码前缀检测）
        for (int i = 0; i < size; i++) {
            byte b = buf.get();
            if (b == 0) {
                run++;
            } else if (b == 1 && run >= 2) {
                // 00 00 01 或 00 00 00 01 起始码：下一个字节是 NAL 头
                if (i + 1 < size) {
                    int nalType = buf.get() & 0x1F;
                    if (nalType == 5) return true;
                    i++;
                }
                run = 0;
            } else {
                run = 0;
            }
        }
        return false;
    }

    // ---- 虚拟显示 + 编码器生命周期 ----

    private synchronized void setup() throws Exception {
        DeviceInfo.Display d = DeviceInfo.getMainDisplay();
        deviceWidth = d.width;
        deviceHeight = d.height;
        deviceRotation = d.rotation;

        int[] size = computeVideoSize(d.width, d.height);
        videoWidth = size[0];
        videoHeight = size[1];
        encWidth = videoWidth * 2;   // 双 YUV420 左右拼接（协议 §3.1）
        encHeight = videoHeight;
        frameBytes = encWidth * encHeight * 3 / 2;

        MediaFormat format = MediaFormat.createVideoFormat(MIME, encWidth, encHeight);
        format.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
        format.setInteger(MediaFormat.KEY_BIT_RATE, bitrate);
        format.setInteger(MediaFormat.KEY_FRAME_RATE, fps);
        format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, I_FRAME_INTERVAL_S);
        format.setInteger("latency", 0);   // KEY_LATENCY
        format.setInteger("priority", 0);  // KEY_PRIORITY：实时优先

        codec = MediaCodec.createEncoderByType(MIME);
        codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        // configure 用调用线程 Looper 建内部 EventHandler（encodeLoop 已 prepare）；
        // setCallback 显式挂 codec-cb 线程，回调不占用编码线程
        codec.setCallback(codecCallback, codecHandler);
        codec.start();
        currentBitrate = bitrate;
        csd0 = null;
        csd1 = null;
        inFlight = 0;
        synchronized (frameLock) {
            freeInputs.clear();
        }

        // 采集 target：默认 GL 重排（SurfaceTexture，帧不出 GPU）；失败回退
        // ImageReader + CPU native 重排
        Surface target;
        if (glRepackEnabled) {
            try {
                glRepack = GlRepack.create(videoWidth, videoHeight, glSink);
                System.out.println("[ScreenEncoder] using GL repack path");
            } catch (Throwable t) {
                System.out.println("[ScreenEncoder] GL repack unavailable (" + t
                        + "), fallback ImageReader+CPU");
                t.printStackTrace(System.out);
                glRepack = null;
            }
        }
        if (glRepack != null) {
            target = glRepack.getSurface();
        } else {
            imageReader = ImageReader.newInstance(videoWidth, videoHeight,
                    PixelFormat.RGBA_8888, 3);
            imageReader.setOnImageAvailableListener(this::onImageAvailable, imageHandler);
            target = imageReader.getSurface();
        }

        // 重排输出缓冲（direct，native 零拷贝读写；容量按 GL readback 需求取大）
        int bufCapacity = Math.max(frameBytes, GlRepack.readBufferBytes(videoWidth, videoHeight));
        freeBufs.clear();
        freeBufs.offer(ByteBuffer.allocateDirect(bufCapacity));
        freeBufs.offer(ByteBuffer.allocateDirect(bufCapacity));
        synchronized (frameLock) {
            pendingFrame = null;
        }

        display = SurfaceControl.createDisplay("tightcast", false);
        Rect contentRect = new Rect(0, 0, d.width, d.height);
        Rect videoRect = new Rect(0, 0, videoWidth, videoHeight);
        SurfaceControl.openTransaction();
        try {
            SurfaceControl.setDisplaySurface(display, target);
            SurfaceControl.setDisplayProjection(display, 0, contentRect, videoRect);
            SurfaceControl.setDisplayLayerStack(display, d.layerStack);
        } finally {
            SurfaceControl.closeTransaction();
        }
        lastRotationCheck = System.currentTimeMillis();
        lastIdrAt = lastRotationCheck;  // 新编码器首帧必为 IDR
    }

    private synchronized void teardown() {
        // 先摘回调再关 reader，避免回调线程读到已失效的尺寸/编码器状态
        if (imageReader != null) {
            imageReader.setOnImageAvailableListener(null, null);
            synchronized (captureLock) {
                // 与 onImageAvailable 互斥：等待进行中的重排完成后再 close，
                // 否则其已 acquire 图像的 gralloc 缓冲被释放 → 重排读野指针崩溃
                imageReader.close();
                imageReader = null;
            }
        }
        // GL 重排资源随 recreate 重建（SurfaceTexture 关联的采集 surface 一并失效）
        if (glRepack != null) {
            glRepack.release();
            glRepack = null;
        }
        if (codec != null) {
            try {
                codec.setCallback(null);  // 先摘回调，防 stop 期间回调打进来
            } catch (Exception ignored) {}
            try {
                codec.stop();
            } catch (Exception ignored) {}
            try {
                codec.release();
            } catch (Exception ignored) {}
            codec = null;
        }
        synchronized (frameLock) {
            freeInputs.clear();
            inFlight = 0;
        }
        if (display != null) {
            try {
                SurfaceControl.destroyDisplay(display);
            } catch (Exception e) {
                System.out.println("[ScreenEncoder] destroyDisplay failed: " + e);
            }
            display = null;
        }
        synchronized (frameLock) {
            if (pendingFrame != null) {
                freeBufs.offer(pendingFrame);
                pendingFrame = null;
            }
        }
    }

    private void recreate() throws Exception {
        teardown();
        setup();
        System.out.println("[ScreenEncoder] recreated " + encWidth + "x" + encHeight);
        Runnable cb = onDisplaySizeChanged;
        if (cb != null) cb.run();
    }

    private boolean displayChanged() {
        try {
            DeviceInfo.Display d = DeviceInfo.getMainDisplay();
            return d.rotation != deviceRotation || d.width != deviceWidth
                    || d.height != deviceHeight;
        } catch (Exception e) {
            return false;
        }
    }

    /** 最长边缩到 maxSize，保持纵横比，偶数对齐（协议要求 W、H 为偶数）。 */
    private int[] computeVideoSize(int w, int h) {
        int longSide = Math.max(w, h);
        if (longSide <= maxSize) {
            return new int[] {w & ~1, h & ~1};
        }
        double scale = (double) maxSize / longSide;
        int vw = ((int) Math.round(w * scale)) & ~1;
        int vh = ((int) Math.round(h * scale)) & ~1;
        return new int[] {vw, vh};
    }

    // ---- SurfaceControl 反射封装：Android 13 静态方法优先，回退 Transaction ----

    private static final class SurfaceControl {
        private static final Class<?> cls;
        private static final boolean useTransaction;
        private static Method mCreateDisplay;
        private static Method mOpenTransaction;
        private static Method mCloseTransaction;
        private static Method mSetDisplaySurface;
        private static Method mSetDisplayProjection;
        private static Method mSetDisplayLayerStack;
        private static Method mDestroyDisplay;
        // Transaction 回退
        private static Class<?> txClass;
        private static Method txSetDisplaySurface;
        private static Method txSetDisplayProjection;
        private static Method txSetDisplayLayerStack;
        private static Method txApply;

        static {
            Class<?> c = null;
            boolean tx = false;
            try {
                c = Class.forName("android.view.SurfaceControl");
                mCreateDisplay = c.getMethod("createDisplay", String.class, boolean.class);
                mOpenTransaction = c.getMethod("openTransaction");
                mCloseTransaction = c.getMethod("closeTransaction");
                mSetDisplaySurface = c.getMethod("setDisplaySurface", IBinder.class,
                        android.view.Surface.class);
                mSetDisplayProjection = c.getMethod("setDisplayProjection", IBinder.class,
                        int.class, Rect.class, Rect.class);
                mSetDisplayLayerStack = c.getMethod("setDisplayLayerStack", IBinder.class,
                        int.class);
                mDestroyDisplay = c.getMethod("destroyDisplay", IBinder.class);
            } catch (Exception e) {
                System.out.println("[ScreenEncoder] static SurfaceControl methods unavailable ("
                        + e + "), falling back to Transaction");
                tx = true;
            }
            if (tx && c != null) {
                try {
                    txClass = Class.forName("android.view.SurfaceControl$Transaction");
                    txSetDisplaySurface = txClass.getMethod("setDisplaySurface", IBinder.class,
                            android.view.Surface.class);
                    txSetDisplayProjection = txClass.getMethod("setDisplayProjection",
                            IBinder.class, int.class, Rect.class, Rect.class);
                    txSetDisplayLayerStack = txClass.getMethod("setDisplayLayerStack",
                            IBinder.class, int.class);
                    txApply = txClass.getMethod("apply");
                } catch (Exception e) {
                    System.out.println("[ScreenEncoder] Transaction reflection failed: " + e);
                }
            }
            cls = c;
            useTransaction = tx;
        }

        static IBinder createDisplay(String name, boolean secure) throws Exception {
            return (IBinder) mCreateDisplay.invoke(null, name, secure);
        }

        static void destroyDisplay(IBinder display) throws Exception {
            mDestroyDisplay.invoke(null, display);
        }

        private static Object transaction; // Transaction 回退实例

        static void openTransaction() throws Exception {
            if (useTransaction) {
                Constructor<?> ctor = txClass.getDeclaredConstructor();
                transaction = ctor.newInstance();
            } else {
                mOpenTransaction.invoke(null);
            }
        }

        static void closeTransaction() throws Exception {
            if (useTransaction) {
                if (transaction != null) {
                    txApply.invoke(transaction);
                    transaction = null;
                }
            } else {
                mCloseTransaction.invoke(null);
            }
        }

        static void setDisplaySurface(IBinder display, Surface surface) throws Exception {
            if (useTransaction) {
                txSetDisplaySurface.invoke(transaction, display, surface);
            } else {
                mSetDisplaySurface.invoke(null, display, surface);
            }
        }

        static void setDisplayProjection(IBinder display, int orientation, Rect layerStackRect,
                Rect displayRect) throws Exception {
            if (useTransaction) {
                txSetDisplayProjection.invoke(transaction, display, orientation, layerStackRect,
                        displayRect);
            } else {
                mSetDisplayProjection.invoke(null, display, orientation, layerStackRect,
                        displayRect);
            }
        }

        static void setDisplayLayerStack(IBinder display, int layerStack) throws Exception {
            if (useTransaction) {
                txSetDisplayLayerStack.invoke(transaction, display, layerStack);
            } else {
                mSetDisplayLayerStack.invoke(null, display, layerStack);
            }
        }
    }
}
