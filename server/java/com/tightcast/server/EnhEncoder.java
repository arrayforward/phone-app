package com.tightcast.server;

import android.media.Image;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * layer 模式增强层编码器（协议 §3.4 v2）：残差 sym 帧（直偏置，128=零残差）
 * 作为普通 I420 帧送第二个 MediaCodec H.264 硬编——实验证明直偏置残差恰在
 * H.264 舒适区（近灰噪声图），而差分伪装不可行（见 docs/layer_test_report.md）。
 *
 * 帧间预测 + 周期 IDR（1s）：迷彩帧静态期高度相似，帧间压缩收益大；
 * 丢帧断链由 client 发 0x07 REQ_ENH_KEYFRAME 恢复。
 *
 * 输出 ch 4 完整消息：[0x57][pts_ms u64be][kind=0x02][enh_flags(bit0=IDR)][AU]，
 * IDR 前拼自身 SPS/PPS（Annex-B）。
 */
public final class EnhEncoder {

    private static final String MIME = "video/avc";
    private static final int ENH_CHANNEL = 4;
    private static final byte ENH_KIND_H264 = 0x02;

    public interface Listener {
        /** 完整 ch 4 消息已组好（编码回调线程，快速返回）。 */
        void onEnhMessage(byte[] packet, boolean isIdr);
    }

    private final int width;
    private final int height;
    private final int fps;
    private final int bitrateCap;   // 硬上限（只降不超）
    private final Listener listener;

    private HandlerThread codecThread;
    private Handler codecHandler;
    private MediaCodec codec;
    private byte[] csd0;
    private byte[] csd1;
    private int currentBitrate;
    private long lastIdrAt;

    private final Object frameLock = new Object();
    private final java.util.ArrayDeque<Integer> freeInputs = new java.util.ArrayDeque<>();
    private int inFlight;
    // 自持 sym 环形缓冲（feed 即拷入，queueInputBuffer 后归还）——调用方缓冲即刻回收
    private final java.util.ArrayDeque<ByteBuffer> symFree = new java.util.ArrayDeque<>();
    private static final int SYM_RING = 3;
    // 最新帧槽（丢旧保新）：增强层赶上是增值、赶不上可丢
    private ByteBuffer pendingFrame;
    private long pendingPtsUs;

    // D8 对匿名类/非静态内部类有内部 NPE bug：回调类一律 static 具名嵌套
    private static final class Cb extends MediaCodec.Callback {
        private final EnhEncoder owner;

        Cb(EnhEncoder owner) {
            this.owner = owner;
        }

        @Override
        public void onInputBufferAvailable(MediaCodec c, int index) {
            synchronized (owner.frameLock) {
                if (c != owner.codec) return;
                owner.freeInputs.add(index);
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
                synchronized (owner) {
                    if (c == owner.codec && index >= 0) {
                        owner.onEncoded(c, index, info);
                        c.releaseOutputBuffer(index, false);
                    }
                }
            } catch (Exception e) {
                System.out.println("[EnhEncoder] onOutput error: " + e);
            }
            owner.tryFeed();
        }

        @Override
        public void onError(MediaCodec c, MediaCodec.CodecException e) {
            System.out.println("[EnhEncoder] codec error: " + e);
        }

        @Override
        public void onOutputFormatChanged(MediaCodec c, MediaFormat format) {
            synchronized (owner) {
                if (c == owner.codec) owner.cacheConfigData(format);
            }
        }
    }

    public EnhEncoder(int w, int h, int fps, int bitrate, Listener listener) {
        this.width = w;
        this.height = h;
        this.fps = fps;
        this.bitrateCap = bitrate;
        this.listener = listener;
        codecThread = new HandlerThread("enh-codec");
        codecThread.start();
        codecHandler = new Handler(codecThread.getLooper());
        // configure 需要调用线程有 Looper（ScreenEncoder 同款坑）：在 handler 线程内执行
        codecHandler.post(this::configureOnThread);
    }

    private void configureOnThread() {
        try {
            MediaFormat format = MediaFormat.createVideoFormat(MIME, width, height);
            format.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                    MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
            format.setInteger(MediaFormat.KEY_BIT_RATE, bitrateCap);
            format.setInteger(MediaFormat.KEY_FRAME_RATE, fps);
            format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);  // 周期 IDR 恢复
            format.setInteger("latency", 0);
            format.setInteger("priority", 0);
            codec = MediaCodec.createEncoderByType(MIME);
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            codec.setCallback(new Cb(this), codecHandler);
            codec.start();
            currentBitrate = bitrateCap;
            lastIdrAt = System.currentTimeMillis();
            for (int i = 0; i < SYM_RING; ++i)
                symFree.offer(ByteBuffer.allocateDirect(width * height * 3 / 2));
            System.out.println("[EnhEncoder] started " + width + "x" + height
                    + " @" + bitrateCap + "bps");
        } catch (Exception e) {
            System.out.println("[EnhEncoder] configure failed: " + e);
            e.printStackTrace(System.out);
            codec = null;
        }
    }

    /** 喂一帧残差 sym（紧凑 I420 字节，W*H*3/2）。忙/环形空则丢（增强可丢）。任意线程。 */
    public void feed(byte[] sym, long ptsUs) {
        synchronized (frameLock) {
            ByteBuffer buf = symFree.poll();
            if (buf == null) return;  // 编码跟不上：丢该帧增强
            buf.clear();
            buf.put(sym);
            buf.flip();
            ByteBuffer old = pendingFrame;
            pendingFrame = buf;
            pendingPtsUs = ptsUs;
            if (old != null) symFree.offer(old);  // 旧 pending 未被编码即被顶替：回收
        }
        tryFeed();
    }

    private void tryFeed() {
        ByteBuffer frame;
        long ptsUs;
        int index;
        MediaCodec c;
        synchronized (frameLock) {
            c = codec;
            if (c == null || inFlight > 0) return;
            if (pendingFrame == null || freeInputs.isEmpty()) return;
            frame = pendingFrame;
            ptsUs = pendingPtsUs;
            pendingFrame = null;
            index = freeInputs.poll();
            inFlight++;
        }
        synchronized (this) {
            try {
                Image input = c.getInputImage(index);
                if (input != null) {
                    Repack.fillInputImage(frame, width, height, input);
                    c.queueInputBuffer(index, 0, width * height * 3 / 2, ptsUs, 0);
                } else {
                    synchronized (frameLock) { if (inFlight > 0) inFlight--; }
                }
            } catch (Exception e) {
                System.out.println("[EnhEncoder] feed error: " + e);
                synchronized (frameLock) { if (inFlight > 0) inFlight--; }
            }
        }
        frame.clear();
        synchronized (frameLock) {
            symFree.offer(frame);
        }
    }

    /** 码率调整（capacity × (1−baseShare)）；变化 >15% 才下发。任意线程。 */
    public synchronized void setBitrate(long bps) {
        MediaCodec c = codec;
        if (c == null || bps <= 0) return;
        if (bps > bitrateCap) bps = bitrateCap;
        if (Math.abs(bps - currentBitrate) * 100 <= (long) currentBitrate * 15) return;
        try {
            Bundle b = new Bundle();
            b.putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, (int) Math.min(bps, Integer.MAX_VALUE));
            c.setParameters(b);
            currentBitrate = (int) bps;
        } catch (Exception e) {
            System.out.println("[EnhEncoder] set bitrate failed (ignored): " + e);
        }
    }

    /** 0x07 REQ_ENH_KEYFRAME：立即出一个增强 IDR（丢帧断链恢复）。任意线程。
     *  MIUI 等编码器可能忽略同步帧请求：>2.5s 未出 IDR 则重建编码器兜底。 */
    public synchronized void requestKeyframe() {
        MediaCodec c = codec;
        if (c == null) return;
        final long reqAt = System.currentTimeMillis();
        try {
            Bundle b = new Bundle();
            b.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
            c.setParameters(b);
        } catch (Exception e) {
            System.out.println("[EnhEncoder] request sync frame failed: " + e);
        }
        // MIUI 兜底：超时未出 IDR → 重建（新编码器首帧必为 IDR）
        codecHandler.postDelayed(() -> {
            synchronized (EnhEncoder.this) {
                if (codec != null && lastIdrAt < reqAt
                        && System.currentTimeMillis() - lastIdrAt > 2400) {
                    System.out.println("[EnhEncoder] no IDR after request, recreating");
                    recreateOnThread();
                }
            }
        }, 2500);
    }

    /** 重建编码器（codec 线程内）：尺寸/码率不变，首帧必为 IDR + 新 CSD。 */
    private void recreateOnThread() {
        MediaCodec c;
        synchronized (this) {
            c = codec;
            codec = null;
        }
        // 监控外 stop/release（同 shutdown 注释的锁序）
        if (c != null) {
            try {
                c.setCallback(null);
            } catch (Exception ignored) {}
            try {
                c.stop();
            } catch (Exception ignored) {}
            try {
                c.release();
            } catch (Exception ignored) {}
        }
        synchronized (frameLock) {
            freeInputs.clear();
            inFlight = 0;
        }
        csd0 = null;
        csd1 = null;
        configureOnThread();
    }

    private void onEncoded(MediaCodec c, int index, MediaCodec.BufferInfo info) {
        if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) return;
        if (info.size == 0) return;
        ByteBuffer buf = c.getOutputBuffer(index);
        if (buf == null) return;
        boolean isIdr = (info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;
        ByteBuffer dup = buf.duplicate();
        dup.position(info.offset);
        dup.limit(info.offset + info.size);
        if (!isIdr && ScreenEncoder.containsIdr(dup, info.size)) isIdr = true;  // MIUI 兜底
        if (isIdr) lastIdrAt = System.currentTimeMillis();

        // [0x57][pts_ms u64be][kind=0x02][enh_flags][csd?][AU]
        int csdLen = (isIdr && csd0 != null && csd1 != null) ? csd0.length + csd1.length : 0;
        byte[] packet = new byte[11 + csdLen + info.size];
        ByteBuffer out = ByteBuffer.wrap(packet).order(ByteOrder.BIG_ENDIAN);
        out.put((byte) 0x57);
        out.putLong(info.presentationTimeUs / 1000);
        out.put(ENH_KIND_H264);
        out.put((byte) (isIdr ? 1 : 0));
        if (csdLen > 0) {
            out.put(csd0);
            out.put(csd1);
        }
        buf.position(info.offset);
        buf.limit(info.offset + info.size);
        out.put(buf);
        listener.onEnhMessage(packet, isIdr);
    }

    private void cacheConfigData(MediaFormat format) {
        ByteBuffer c0 = format.getByteBuffer("csd-0");
        ByteBuffer c1 = format.getByteBuffer("csd-1");
        if (c0 != null && c1 != null) {
            csd0 = toArray(c0);
            csd1 = toArray(c1);
            System.out.println("[EnhEncoder] cached csd-0=" + csd0.length + "B csd-1="
                    + csd1.length + "B");
        }
    }

    private static byte[] toArray(ByteBuffer buf) {
        ByteBuffer dup = buf.duplicate();
        byte[] arr = new byte[dup.remaining()];
        dup.get(arr);
        return arr;
    }

    public void shutdown() {
        MediaCodec c;
        synchronized (this) {
            c = codec;
            codec = null;
        }
        synchronized (frameLock) {
            pendingFrame = null;
            symFree.clear();
            freeInputs.clear();
            inFlight = 0;
        }
        // 锁序铁律：监控外 stop/release——持 EnhEncoder 监视器调 codec.stop()
        // 会与"回调线程 onOutputBufferAvailable → tryFeed 取 synchronized(this)"
        // 互锁（stop 等回调返回，回调等监视器）——SET_FORMAT 切换卡死的实测根因
        if (c != null) {
            try {
                c.setCallback(null);
            } catch (Exception ignored) {}
            try {
                c.stop();
            } catch (Exception ignored) {}
            try {
                c.release();
            } catch (Exception ignored) {}
        }
        if (codecThread != null) {
            codecThread.quitSafely();
            codecThread = null;
            codecHandler = null;
        }
    }
}
