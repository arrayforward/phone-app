package com.tightcast.server;

import android.media.Image;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Handler;
import android.os.HandlerThread;

import java.nio.ByteBuffer;
import java.util.ArrayDeque;

/**
 * layer 模式基础层闭环解码器（协议 docs/protocol.md §3.4）：本地解码基础层
 * 码流得重建帧，供残差计算。H.264 解码是确定性的——同一码流在 client 解码器
 * 重建出逐字节相同的帧，故残差必须相对本地重建（而非原始帧）计算。
 *
 * 用法：构造即异步 configure（csd 来自编码器 onOutputFormatChanged）；
 * feed() 喂**实际已发送**的 Annex-B AU（IDR 含内联 SPS/PPS 亦可）；
 * 重建帧经 Listener.onOutput 回调（解码回调线程），回调返回后输出缓冲即释放，
 * 监听方须在回调内拷贝所需数据。
 *
 * 关键约束：输入只能丢在"未送"侧——若某帧已发给 client 而本解码器漏解，
 * 之后到下一 IDR 前的重建链与 client 不一致，算出的残差合成会损坏画面。
 * 因此输入积压溢出时清空待喂队列并回调 onOverflow()：调用方应请求关键帧，
 * 并在下一个 IDR 喂入前抑制增强层发送。
 */
public final class BaseDecoder {

    private static final String MIME = "video/avc";
    private static final int MAX_PENDING_INPUT = 16;

    public interface Listener {
        /**
         * 重建帧输出（解码回调线程）。img 仅在回调期间有效。
         * 编码帧按 mb 对齐（如 886→896），cropL/cropT/cropW/cropH 为输出 format
         * 的 crop 可视区——监听方只应拷可视区（右/下边缘是填充垃圾）。
         */
        void onOutput(Image img, long ptsUs, int cropL, int cropT, int cropW, int cropH);
        /** 输入积压溢出（待喂队列已清空，参考链断裂直到下一 IDR）。 */
        void onOverflow();
    }

    private final int width;
    private final int height;
    private final byte[] csd0;
    private final byte[] csd1;
    private final Listener listener;

    private HandlerThread thread;
    private Handler handler;
    private MediaCodec codec;

    private static final class Au {
        byte[] data;
        long ptsUs;
    }

    private final Object lock = new Object();
    private final ArrayDeque<Au> pending = new ArrayDeque<>();
    private final ArrayDeque<Integer> freeInputs = new ArrayDeque<>();
    private volatile boolean running = true;

    // D8 对匿名类/非静态内部类有内部 NPE bug：回调类一律 static 具名嵌套
    private static final class Cb extends MediaCodec.Callback {
        private final BaseDecoder owner;

        Cb(BaseDecoder owner) {
            this.owner = owner;
        }

        @Override
        public void onInputBufferAvailable(MediaCodec c, int index) {
            synchronized (owner.lock) {
                if (c != owner.codec) return;
                owner.freeInputs.add(index);
            }
            owner.drainInputs();
        }

        @Override
        public void onOutputBufferAvailable(MediaCodec c, int index,
                                            MediaCodec.BufferInfo info) {
            Image img = null;
            try {
                synchronized (owner.lock) {
                    if (!owner.running || c != owner.codec) return;
                }
                img = c.getOutputImage(index);
                if (img != null && info.size > 0) {
                    // crop 可视区（编码帧 mb 对齐填充；无 crop 键 = 全幅）
                    int cl = 0, ct = 0, cw = owner.width, ch = owner.height;
                    MediaFormat of = c.getOutputFormat();
                    if (of != null && of.containsKey("crop-left")) {
                        cl = of.getInteger("crop-left");
                        ct = of.getInteger("crop-top");
                        cw = of.getInteger("crop-right") - cl + 1;
                        ch = of.getInteger("crop-bottom") - ct + 1;
                    }
                    owner.listener.onOutput(img, info.presentationTimeUs, cl, ct, cw, ch);
                }
            } catch (Exception e) {
                System.out.println("[BaseDecoder] onOutput error: " + e);
            } finally {
                try {
                    c.releaseOutputBuffer(index, false);
                } catch (Exception ignored) {}
            }
        }

        @Override
        public void onError(MediaCodec c, MediaCodec.CodecException e) {
            System.out.println("[BaseDecoder] codec error: " + e);
        }

        @Override
        public void onOutputFormatChanged(MediaCodec c, MediaFormat format) {
            System.out.println("[BaseDecoder] output format: " + format);
        }
    }

    public BaseDecoder(int w, int h, byte[] csd0, byte[] csd1, Listener listener) {
        this.width = w;
        this.height = h;
        this.csd0 = csd0;
        this.csd1 = csd1;
        this.listener = listener;
        thread = new HandlerThread("base-dec");
        thread.start();
        handler = new Handler(thread.getLooper());
        // configure 需要调用线程有 Looper（ScreenEncoder 同款坑）：在 handler 线程内执行
        handler.post(this::configureOnThread);
    }

    private void configureOnThread() {
        try {
            MediaFormat format = MediaFormat.createVideoFormat(MIME, width, height);
            format.setByteBuffer("csd-0", ByteBuffer.wrap(csd0));
            format.setByteBuffer("csd-1", ByteBuffer.wrap(csd1));
            format.setInteger("low-latency", 1);  // KEY_LOW_LATENCY（不支持的厂商忽略）
            codec = MediaCodec.createDecoderByType(MIME);
            codec.configure(format, null, null, 0);
            codec.setCallback(new Cb(this), handler);
            codec.start();
            System.out.println("[BaseDecoder] started " + width + "x" + height);
        } catch (Exception e) {
            System.out.println("[BaseDecoder] configure failed: " + e);
            e.printStackTrace(System.out);
            codec = null;
        }
    }

    /** 喂入一个实际已发送的 Annex-B AU。任意线程可调用。 */
    public void feed(byte[] au, long ptsUs) {
        if (au == null || au.length == 0) return;
        boolean overflow = false;
        synchronized (lock) {
            if (!running) return;
            if (pending.size() >= MAX_PENDING_INPUT) {
                // 积压溢出：清空待喂队列（参考链断裂直到下一 IDR），通知调用方
                pending.clear();
                overflow = true;
            }
            Au a = new Au();
            a.data = au;
            a.ptsUs = ptsUs;
            pending.add(a);
        }
        if (overflow) {
            System.out.println("[BaseDecoder] input backlog overflow, reset until next IDR");
            listener.onOverflow();
        }
        drainInputs();
    }

    private void drainInputs() {
        for (;;) {
            int index;
            Au a;
            MediaCodec c;
            synchronized (lock) {
                c = codec;
                if (!running || c == null || pending.isEmpty() || freeInputs.isEmpty()) return;
                index = freeInputs.poll();
                a = pending.poll();
            }
            try {
                ByteBuffer buf = c.getInputBuffer(index);
                if (buf == null || buf.capacity() < a.data.length) {
                    synchronized (lock) {
                        pending.clear();  // 装不下（不应发生）：等同溢出处理
                    }
                    listener.onOverflow();
                    return;
                }
                buf.clear();
                buf.put(a.data);
                c.queueInputBuffer(index, 0, a.data.length, a.ptsUs, 0);
            } catch (Exception e) {
                System.out.println("[BaseDecoder] queue input failed: " + e);
                return;
            }
        }
    }

    public void shutdown() {
        running = false;
        MediaCodec c;
        synchronized (lock) {
            c = codec;
            codec = null;
            pending.clear();
            freeInputs.clear();
        }
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
        if (thread != null) {
            thread.quitSafely();
            thread = null;
            handler = null;
        }
    }
}
