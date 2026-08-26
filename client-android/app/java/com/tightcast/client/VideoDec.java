package com.tightcast.client;

import android.media.Image;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Handler;
import android.os.HandlerThread;

import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.HashMap;
import java.util.Map;

/**
 * 视频解码器（Java MediaCodec buffer 模式）：NDK AImageReader 作解码输出在
 * 华为（EMUI）上 configure 失败（-10000），buffer 模式 + getOutputImage 是
 * 各厂商最兼容的路径（server 侧已验证）。帧数据经 JNI 回 native 排产。
 *
 * 一个实例解一条流：layer=0 基础层（ch0），layer=1 增强层（ch4）。
 * 首个喂入帧必须是带 SPS/PPS 的 IDR（csd 不配，走内联 Annex-B）。
 * wire pts_ms 经 presentationTimeUs 透传；flags（bit1 ycocg / bit2 single）
 * 按 pts 在 Java 侧留底随输出返回。
 */
public final class VideoDec {

    private static final String MIME = "video/avc";
    private static final int MAX_PENDING = 8;

    private final int layer;
    private HandlerThread thread;
    private Handler handler;
    private MediaCodec codec;

    private static final class Au {
        byte[] data;
        long ptsMs;
        int flags;
    }

    private final Object lock = new Object();
    private final ArrayDeque<Au> pending = new ArrayDeque<>();
    private final ArrayDeque<Integer> freeInputs = new ArrayDeque<>();
    private final Map<Long, Integer> flagsByPts = new HashMap<>();
    private volatile boolean running = true;
    // 诊断计数（华为 ROM 抑制应用 logcat → 经 UI 状态条观测）
    private long fedCount;
    private long outCount;
    private volatile String lastError = "";
    // 输出 format 的 crop（onOutputFormatChanged 时缓存；编码帧 mb 对齐填充）
    private int cropL, cropT, cropW = -1, cropH = -1;
    private int cfgW, cfgH;  // 已配置尺寸（SPS 显示尺寸；crop 可能因 mb 对齐再报）

    /** 诊断状态行（UI 状态条用）。 */
    public String status() {
        synchronized (lock) {
            return "dec" + layer + "[fed=" + fedCount + " out=" + outCount
                    + " pend=" + pending.size() + " free=" + freeInputs.size()
                    + " crop=" + cropW + "x" + cropH
                    + (lastError.isEmpty() ? "" : " err=" + lastError) + "]";
        }
    }

    // D8 对匿名类/非静态内部类有内部 NPE bug：一律 static 具名嵌套
    private static final class Cb extends MediaCodec.Callback {
        private final VideoDec owner;

        Cb(VideoDec owner) {
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
                    owner.onFrame(img, info.presentationTimeUs);
                }
            } catch (Exception e) {
                Log2File.log("[VideoDec] onOutput error: " + e);
            } finally {
                try {
                    c.releaseOutputBuffer(index, false);
                } catch (Exception ignored) {}
            }
        }

        @Override
        public void onError(MediaCodec c, MediaCodec.CodecException e) {
            Log2File.log("[VideoDec] layer" + owner.layer + " codec error: " + e);
        }

        @Override
        public void onOutputFormatChanged(MediaCodec c, MediaFormat format) {
            owner.onFormat(format);
        }
    }

    public VideoDec(int layer) {
        this.layer = layer;
        thread = new HandlerThread("video-dec-" + layer);
        thread.start();
        handler = new Handler(thread.getLooper());
        // 惰性 configure：首个 IDR（内联 SPS/PPS）到达时才建解码器——
        // 占位尺寸/缺 csd 的 configure 在华为上直接 0x80001001 失败
    }

    /** Annex-B AU 起始码扫描，返回 NAL 区间列表（[begin,end)，begin 为 NAL 头）。 */
    private static java.util.List<int[]> splitAnnexB(byte[] data) {
        java.util.List<int[]> out = new java.util.ArrayList<>();
        int i = 0;
        while (i < data.length) {
            int sc = 0;
            if (i + 3 <= data.length && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) sc = 3;
            else if (i + 4 <= data.length && data[i] == 0 && data[i + 1] == 0
                    && data[i + 2] == 0 && data[i + 3] == 1) sc = 4;
            if (sc == 0) {
                i++;
                continue;
            }
            int nb = i + sc, ne = data.length;
            for (int j = nb; j + 3 <= data.length; ++j) {
                if ((data[j] == 0 && data[j + 1] == 0 && data[j + 2] == 1)
                        || (j + 4 <= data.length && data[j] == 0 && data[j + 1] == 0
                            && data[j + 2] == 0 && data[j + 3] == 1)) {
                    ne = j;
                    break;
                }
            }
            if (nb < ne) out.add(new int[] {nb, ne});
            i = ne;
        }
        return out;
    }

    /** Exp-Golomb 位读（去防竞争字节 00 00 03）。 */
    private static final class BitReader {
        private final byte[] rbsp;
        private int pos;

        BitReader(byte[] data, int off, int len) {
            byte[] tmp = new byte[len];
            int n = 0;
            for (int i = 0; i < len; ++i) {
                byte b = data[off + i];
                if (n >= 2 && tmp[n - 1] == 0 && tmp[n - 2] == 0 && b == 3) continue;
                tmp[n++] = b;
            }
            rbsp = java.util.Arrays.copyOf(tmp, n);
        }

        int u1() {
            if (pos >= rbsp.length * 8) return 0;
            int b = (rbsp[pos >> 3] >> (7 - (pos & 7))) & 1;
            ++pos;
            return b;
        }

        long ue() {
            int zeros = 0;
            while (u1() == 0 && zeros < 32) ++zeros;
            long suffix = 0;
            for (int i = 0; i < zeros; ++i) suffix = (suffix << 1) | u1();
            return ((1L << zeros) - 1) + suffix;
        }

        long se() {
            long v = ue();
            return (v & 1) != 0 ? (v + 1) / 2 : -(v / 2);
        }
    }

    /** 解析 SPS（不含 NAL 头）得显示尺寸（含 frame_crop）；失败返回 null。 */
    private static int[] parseSpsDims(byte[] data, int off, int len) {
        if (len < 4) return null;
        BitReader br = new BitReader(data, off, len);
        int profile = 0;
        for (int i = 0; i < 8; ++i) profile = (profile << 1) | br.u1();
        for (int i = 0; i < 8; ++i) br.u1();  // constraint flags + reserved
        for (int i = 0; i < 8; ++i) br.u1();  // level_idc
        br.ue();  // seq_parameter_set_id
        if (profile == 100 || profile == 110 || profile == 122 || profile == 244
                || profile == 44 || profile == 83 || profile == 86 || profile == 118
                || profile == 128 || profile == 138 || profile == 139 || profile == 134
                || profile == 135) {
            long chroma = br.ue();
            if (chroma == 3) br.u1();
            br.ue();  // bit_depth_luma_minus8
            br.ue();  // bit_depth_chroma_minus8
            br.u1();  // qpprime_y_zero_transform_bypass_flag
            if (br.u1() != 0) return null;  // seq_scaling_matrix_present：不解析
        }
        br.ue();  // log2_max_frame_num_minus4
        long poc = br.ue();
        if (poc == 0) {
            br.ue();
        } else if (poc == 1) {
            br.u1();
            br.se();
            br.se();
            long n = br.ue();
            if (n > 255) return null;
            for (long i = 0; i < n; ++i) br.se();
        }
        br.ue();  // max_num_ref_frames
        br.u1();  // gaps_in_frame_num_value_allowed_flag
        long wmbs = br.ue();
        long hmap = br.ue();
        if (wmbs > 1023 || hmap > 1023) return null;
        int frameOnly = br.u1();
        if (frameOnly == 0) br.u1();
        br.u1();  // direct_8x8_inference_flag
        int cw = (int) (wmbs + 1) * 16;
        int ch = (int) (hmap + 1) * 16 * (frameOnly != 0 ? 1 : 2);
        if (br.u1() != 0) {  // frame_cropping_flag
            long left = br.ue(), right = br.ue(), top = br.ue(), bottom = br.ue();
            int subX = 2, subY = frameOnly != 0 ? 2 : 4;
            cw -= (int) ((left + right) * subX);
            ch -= (int) ((top + bottom) * subY);
        }
        if (cw <= 0 || ch <= 0) return null;
        return new int[] {cw, ch};
    }

    /** 首个 IDR（或尺寸变化 IDR）驱动惰性 configure / 重建。返回 false=失败。 */
    private boolean ensureConfigured(byte[] au) {
        int[] dims = null;
        byte[] sps = null, pps = null;
        for (int[] range : splitAnnexB(au)) {
            int type = au[range[0]] & 0x1F;
            if (type == 7 && sps == null) {
                sps = java.util.Arrays.copyOfRange(au, range[0], range[1]);
                dims = parseSpsDims(au, range[0] + 1, range[1] - range[0] - 1);
            } else if (type == 8 && pps == null) {
                pps = java.util.Arrays.copyOfRange(au, range[0], range[1]);
            }
        }
        if (sps == null || pps == null || dims == null) {
            lastError = "no sps/pps or parse failed";
            return false;
        }
        MediaCodec existing;
        synchronized (lock) {
            existing = codec;
        }
        if (existing != null && dims[0] == cfgW && dims[1] == cfgH) return true;
        // （重建：尺寸变化）先关旧解码器
        if (existing != null) {
            try {
                existing.setCallback(null);
                existing.stop();
                existing.release();
            } catch (Exception ignored) {}
            synchronized (lock) {
                codec = null;
                freeInputs.clear();
            }
        }
        final byte[] csd0 = sps, csd1 = pps;
        final int w = dims[0], h = dims[1];
        // configure 需要调用线程 Looper：切到 handler 线程同步执行
        final boolean[] ok = {false};
        java.util.concurrent.CountDownLatch latch = new java.util.concurrent.CountDownLatch(1);
        handler.post(() -> {
            try {
                // 华为 Kirin 解码器两个坑：① configure 的 width/height 要 mb 对齐
                // （886 直接 0x80001001；真实尺寸由 SPS/输出 crop 决定）；
                // ② csd 必须 direct ByteBuffer（heap 缓冲被拒）
                int wa = (w + 15) & ~15, ha = (h + 15) & ~15;
                MediaFormat format = MediaFormat.createVideoFormat(MIME, wa, ha);
                ByteBuffer d0 = ByteBuffer.allocateDirect(csd0.length);
                d0.put(csd0);
                d0.flip();
                ByteBuffer d1 = ByteBuffer.allocateDirect(csd1.length);
                d1.put(csd1);
                d1.flip();
                format.setByteBuffer("csd-0", d0);
                format.setByteBuffer("csd-1", d1);
                MediaCodec c = MediaCodec.createDecoderByType(MIME);
                c.configure(format, null, null, 0);
                c.setCallback(new Cb(VideoDec.this), handler);
                c.start();
                synchronized (lock) {
                    codec = c;
                }
                cfgW = w;
                cfgH = h;
                cropW = w;
                cropH = h;
                cropL = cropT = 0;
                ok[0] = true;
                Log2File.log("[VideoDec] layer" + layer + " configured " + w + "x" + h);
            } catch (Exception e) {
                Log2File.log("[VideoDec] layer" + layer + " configure failed: " + e);
                lastError = "configure " + e;
            }
            latch.countDown();
        });
        try {
            latch.await(2, java.util.concurrent.TimeUnit.SECONDS);
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }
        return ok[0];
    }

    private void onFormat(MediaFormat f) {
        if (f != null && f.containsKey("crop-left")) {
            cropL = f.getInteger("crop-left");
            cropT = f.getInteger("crop-top");
            cropW = f.getInteger("crop-right") - cropL + 1;
            cropH = f.getInteger("crop-bottom") - cropT + 1;
        } else if (f != null) {
            cropL = cropT = 0;
            cropW = f.getInteger(MediaFormat.KEY_WIDTH);
            cropH = f.getInteger(MediaFormat.KEY_HEIGHT);
        }
        Log2File.log("[VideoDec] layer" + layer + " format: " + f
                + " -> crop " + cropW + "x" + cropH);
    }

    /** 喂一个 Annex-B AU（native 解码线程调用）。首个必须是含 SPS/PPS 的 IDR。
     *  积压满 / 配置失败 → false（调用方按丢帧处理）。 */
    public boolean feed(byte[] au, long ptsMs, int flags) {
        if (au == null || au.length == 0) return false;
        boolean isIdr = (flags & 1) != 0;
        synchronized (lock) {
            if (!running) return false;
        }
        if (isIdr && !ensureConfigured(au)) return false;
        synchronized (lock) {
            if (!running || codec == null) return false;
            if (pending.size() >= MAX_PENDING) return false;
            Au a = new Au();
            a.data = au;
            a.ptsMs = ptsMs;
            a.flags = flags;
            pending.add(a);
            flagsByPts.put(ptsMs, flags);
        }
        drainInputs();
        return true;
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
                    lastError = "inbuf null/small";
                    return;
                }
                buf.clear();
                buf.put(a.data);
                // pts 透传：presentationTimeUs 直接承载 wire pts_ms
                c.queueInputBuffer(index, 0, a.data.length, a.ptsMs, 0);
                fedCount++;
            } catch (Exception e) {
                Log2File.log("[VideoDec] queue input failed: " + e);
                lastError = "queue " + e;
                return;
            }
        }
    }

    private void onFrame(Image img, long ptsMs) {
        outCount++;
        int w = cropW > 0 ? cropW : img.getWidth();
        int h = cropH > 0 ? cropH : img.getHeight();
        int flags;
        synchronized (lock) {
            Integer f = flagsByPts.remove(ptsMs);
            flags = f != null ? f : 0;
            if (flagsByPts.size() > 32) flagsByPts.clear();  // 防御：丢帧后的残留
        }
        Image.Plane[] planes = img.getPlanes();
        if (planes.length < 3) return;
        NativeClient.nativeOnDecodedFrame(layer, ptsMs, flags,
                sliceOff(planes[0].getBuffer(),
                        cropT * planes[0].getRowStride() + cropL * planes[0].getPixelStride()),
                planes[0].getRowStride(), planes[0].getPixelStride(),
                sliceOff(planes[1].getBuffer(),
                        cropT / 2 * planes[1].getRowStride()
                                + cropL / 2 * planes[1].getPixelStride()),
                planes[1].getRowStride(), planes[1].getPixelStride(),
                sliceOff(planes[2].getBuffer(),
                        cropT / 2 * planes[2].getRowStride()
                                + cropL / 2 * planes[2].getPixelStride()),
                planes[2].getRowStride(), planes[2].getPixelStride(),
                w, h);
    }

    private static ByteBuffer sliceOff(ByteBuffer buf, int off) {
        ByteBuffer d = buf.duplicate();
        d.position(0);
        d.limit(d.capacity());
        if (off < 0 || off > d.capacity()) off = 0;
        d.position(off);
        return d.slice();
    }

    public void shutdown() {
        running = false;
        MediaCodec c;
        synchronized (lock) {
            c = codec;
            codec = null;
            pending.clear();
            freeInputs.clear();
            flagsByPts.clear();
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
