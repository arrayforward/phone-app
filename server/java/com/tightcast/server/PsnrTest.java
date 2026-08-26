package com.tightcast.server;

import android.media.Image;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.os.Looper;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.file.Files;

/**
 * PSNR 对比测试入口（一次性工具，由 Server --psnr-test 分支调用）：
 * 同一张 RGBA 基准帧分别走
 *   - 单 YUV420（BT.601 4:2:0 亚采样）编码 W×H   @6Mbps        → single_6M.h264
 *   - 双 YUV420 raw（协议 §3.1）编码 2W×H @6M/9M/12Mbps        → dual_{6,9,12}M.h264
 *   - 双 YUV420 YCoCg（协议 §3.2）编码 2W×H @6M/9M/12Mbps      → dual_ycocg_{6,9,12}M.h264
 * 四条路径各建一个 MediaCodec buffer-input 硬编，只编一帧（首帧必为 IDR），
 * SPS/PPS（Annex-B）写在文件开头，供 PC 端 vpl_decode 解码后算 PSNR。
 *
 * 用法（app_process）：--psnr-test <ref.rgba 路径> <输出目录>
 * ref.rgba 为 W×H RGBA_8888 紧凑字节流（886×1920×4 = 6804480 字节）。
 */
public final class PsnrTest {

    private static final String MIME = "video/avc";
    private static final int W = 886;
    private static final int H = 1920;
    private static final int FPS = 30;
    private static final long TIMEOUT_US = 10_000;

    private PsnrTest() {}

    // ---- 方案A 验证实验（--layer-test）：残差直偏置 vs 差分伪装的硬件编码对比 ----

    private static final int[] LAYER_BASE_BPS = {2_000_000, 4_000_000};
    private static final int[] LAYER_ENH_BPS = {500_000, 1_000_000, 2_000_000, 4_000_000, 8_000_000};

    /**
     * 方案A 实验（docs/android_client_plan.md §3）：
     *   基准帧 → single I420 基础层编码 → 同步本地解码得重建帧 → 残差 sym
     *   → 直偏置 / 差分伪装两路各 5 档码率编码 → 输出码流与计时。
     * 产物（out_dir）：
     *   layer_orig.i420            原始 I420（合成真值）
     *   layer_base_{Bb}.h264       基础层码流（host 解码 = 跨解码器重建校验）
     *   layer_recon_{Bb}.i420      手机侧 MediaCodec 解码的重建帧
     *   layer_direct_{Bb}_{Be}.h264  残差直偏置编码
     *   layer_camo_{Bb}_{Be}.h264    残差差分伪装编码
     * 日志（host 汇总解析）：[LayerTest] file=... bytes=... enc_ms=... /
     *   [LayerTest] rice_bytes=... / [LayerTest] residual_ms=... camo_ms=...
     */
    public static int runLayerTest(String rgbaPath, String outDir) {
        byte[] rgba;
        try {
            rgba = Files.readAllBytes(new File(rgbaPath).toPath());
        } catch (IOException e) {
            System.out.println("[LayerTest] read " + rgbaPath + " failed: " + e);
            return 1;
        }
        if (rgba.length != W * H * 4) {
            System.out.println("[LayerTest] bad rgba size " + rgba.length);
            return 1;
        }
        File dir = new File(outDir);
        if (!dir.isDirectory() && !dir.mkdirs()) {
            System.out.println("[LayerTest] mkdir " + outDir + " failed");
            return 1;
        }
        Looper.prepare();  // MediaCodec.configure 需要调用线程 Looper

        ByteBuffer orig = ByteBuffer.allocateDirect(W * H * 3 / 2);
        Repack.rgbaToI420Array(rgba, W, H, orig);
        writeFile(new File(dir, "layer_orig.i420"), orig, W * H * 3 / 2);

        int rc = 0;
        for (int bb : LAYER_BASE_BPS) {
            String bbTag = bpsTag(bb);
            // 1. 基础层编码 + 本地闭环解码
            byte[] baseAu = encodeOneBytes(orig, W, H, bb, "layer_base_" + bbTag);
            if (baseAu == null) { rc = 1; continue; }
            writeFile(new File(dir, "layer_base_" + bbTag + ".h264"), baseAu);
            ByteBuffer recon = decodeOneFrame(baseAu, W, H);
            if (recon == null) { rc = 1; continue; }
            writeFile(new File(dir, "layer_recon_" + bbTag + ".i420"), recon, W * H * 3 / 2);

            // 2. 残差 sym（compact recon → 三个平面 slice）
            int ySize = W * H, cSize = (W / 2) * (H / 2);
            long t0 = System.nanoTime();
            byte[] sym = Layered.nativeComputeSym(slice0(orig), W, H,
                    sliceAt(recon, 0, ySize), W, 1,
                    sliceAt(recon, ySize, cSize), W / 2, 1,
                    sliceAt(recon, ySize + cSize, cSize), W / 2, 1);
            long residualMs = (System.nanoTime() - t0) / 1_000_000;
            if (sym == null) {
                System.out.println("[LayerTest] computeSym failed bb=" + bbTag);
                rc = 1;
                continue;
            }

            // 3. 迷彩正向（模 256 差分）
            t0 = System.nanoTime();
            byte[] camo = Layered.nativeCamouflage(sym, W, H, false);
            long camoMs = (System.nanoTime() - t0) / 1_000_000;
            System.out.println("[LayerTest] bb=" + bbTag + " residual_ms=" + residualMs
                    + " camo_ms=" + camoMs);

            // 4. Rice 无损对照（只记体积与耗时，不出文件）
            t0 = System.nanoTime();
            byte[] rice = Layered.nativeComputeEnhancement(slice0(orig), W, H,
                    sliceAt(recon, 0, ySize), W, 1,
                    sliceAt(recon, ySize, cSize), W / 2, 1,
                    sliceAt(recon, ySize + cSize, cSize), W / 2, 1, 16L * 1024 * 1024);
            long riceMs = (System.nanoTime() - t0) / 1_000_000;
            System.out.println("[LayerTest] bb=" + bbTag + " rice_bytes="
                    + (rice == null ? -1 : rice.length) + " rice_ms=" + riceMs);

            // 5. 两路残差各 5 档硬件编码
            ByteBuffer symBuf = ByteBuffer.allocateDirect(sym.length);
            symBuf.put(sym);
            ByteBuffer camoBuf = ByteBuffer.allocateDirect(camo.length);
            camoBuf.put(camo);
            for (int be : LAYER_ENH_BPS) {
                String tag = "layer_direct_" + bbTag + "_" + bpsTag(be);
                byte[] au = encodeOneBytes(symBuf, W, H, be, tag);
                if (au == null) { rc = 1; continue; }
                writeFile(new File(dir, tag + ".h264"), au);
                tag = "layer_camo_" + bbTag + "_" + bpsTag(be);
                au = encodeOneBytes(camoBuf, W, H, be, tag);
                if (au == null) { rc = 1; continue; }
                writeFile(new File(dir, tag + ".h264"), au);
            }
        }
        System.out.println("[LayerTest] " + (rc == 0 ? "ALL DONE" : "FAILED rc=" + rc));
        return rc;
    }

    private static String bpsTag(int bps) {
        return bps % 1_000_000 == 0 ? (bps / 1_000_000) + "M" : (bps / 1000) + "k";
    }

    private static ByteBuffer sliceAt(ByteBuffer buf, int off, int len) {
        ByteBuffer d = buf.duplicate();
        d.position(off);
        d.limit(off + len);
        return d.slice();
    }

    private static void writeFile(File f, ByteBuffer buf, int len) {
        try (FileOutputStream fos = new FileOutputStream(f)) {
            ByteBuffer d = buf.duplicate();
            d.position(0);
            d.limit(len);
            byte[] arr = new byte[len];
            d.get(arr);
            fos.write(arr);
        } catch (IOException e) {
            System.out.println("[LayerTest] write " + f.getName() + " failed: " + e);
        }
    }

    private static void writeFile(File f, byte[] data) {
        try (FileOutputStream fos = new FileOutputStream(f)) {
            fos.write(data);
        } catch (IOException e) {
            System.out.println("[LayerTest] write " + f.getName() + " failed: " + e);
        }
    }

    /** 编一帧（首帧必为 IDR）返回完整 Annex-B（SPS/PPS + AU）；失败返回 null。 */
    private static byte[] encodeOneBytes(ByteBuffer i420, int encW, int encH, int bitrate,
                                         String tag) {
        final int frameBytes = encW * encH * 3 / 2;
        MediaCodec codec = null;
        java.io.ByteArrayOutputStream bos = new java.io.ByteArrayOutputStream();
        long t0 = System.nanoTime();
        try {
            MediaFormat format = MediaFormat.createVideoFormat(MIME, encW, encH);
            format.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                    MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
            format.setInteger(MediaFormat.KEY_BIT_RATE, bitrate);
            format.setInteger(MediaFormat.KEY_FRAME_RATE, FPS);
            format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 10);
            format.setInteger(MediaFormat.KEY_BITRATE_MODE,
                    MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR);
            format.setInteger("latency", 0);
            format.setInteger("priority", 0);

            codec = MediaCodec.createEncoderByType(MIME);
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            codec.start();

            int inIdx = codec.dequeueInputBuffer(TIMEOUT_US);
            if (inIdx < 0) throw new IllegalStateException("no input buffer");
            Image input = codec.getInputImage(inIdx);
            if (input == null) throw new IllegalStateException("getInputImage null");
            Repack.fillInputImage(i420, encW, encH, input);
            codec.queueInputBuffer(inIdx, 0, frameBytes, 0, 0);
            inIdx = codec.dequeueInputBuffer(TIMEOUT_US);
            if (inIdx < 0) throw new IllegalStateException("no input buffer for EOS");
            codec.queueInputBuffer(inIdx, 0, 0, 1, MediaCodec.BUFFER_FLAG_END_OF_STREAM);

            boolean eos = false;
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            while (!eos) {
                int outIdx = codec.dequeueOutputBuffer(info, TIMEOUT_US);
                if (outIdx == MediaCodec.INFO_TRY_AGAIN_LATER) {
                    continue;
                } else if (outIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    MediaFormat f = codec.getOutputFormat();
                    ByteBuffer c0 = f.getByteBuffer("csd-0");
                    ByteBuffer c1 = f.getByteBuffer("csd-1");
                    if (c0 != null && c1 != null) {
                        bos.write(toArray(c0), 0, c0.remaining());
                        bos.write(toArray(c1), 0, c1.remaining());
                    }
                } else if (outIdx >= 0) {
                    ByteBuffer buf = codec.getOutputBuffer(outIdx);
                    if (buf != null && info.size > 0
                            && (info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0) {
                        bos.write(toArray(buf, info), 0, info.size);
                    }
                    codec.releaseOutputBuffer(outIdx, false);
                    eos = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                }
            }
            long encMs = (System.nanoTime() - t0) / 1_000_000;
            System.out.println("[LayerTest] file=" + tag + ".h264 bytes=" + bos.size()
                    + " enc_ms=" + encMs);
            return bos.toByteArray();
        } catch (Exception e) {
            System.out.println("[LayerTest] encode " + tag + " failed: " + e);
            e.printStackTrace(System.out);
            return null;
        } finally {
            if (codec != null) {
                try {
                    codec.stop();
                } catch (Exception ignored) {}
                try {
                    codec.release();
                } catch (Exception ignored) {}
            }
        }
    }

    /** 同步解码一帧（闭环重建用）：Annex-B（含内联 SPS/PPS）→ 紧凑 planar I420。 */
    private static ByteBuffer decodeOneFrame(byte[] annexb, int w, int h) {
        MediaCodec codec = null;
        long t0 = System.nanoTime();
        try {
            MediaFormat format = MediaFormat.createVideoFormat(MIME, w, h);
            format.setInteger("low-latency", 1);
            codec = MediaCodec.createDecoderByType(MIME);
            codec.configure(format, null, null, 0);
            codec.start();

            int inIdx = codec.dequeueInputBuffer(TIMEOUT_US * 100);
            if (inIdx < 0) throw new IllegalStateException("no input buffer");
            ByteBuffer inBuf = codec.getInputBuffer(inIdx);
            if (inBuf == null || inBuf.capacity() < annexb.length)
                throw new IllegalStateException("input buffer too small");
            inBuf.clear();
            inBuf.put(annexb);
            codec.queueInputBuffer(inIdx, 0, annexb.length, 0, 0);

            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            for (int tries = 0; tries < 1000; ++tries) {
                int outIdx = codec.dequeueOutputBuffer(info, TIMEOUT_US);
                if (outIdx == MediaCodec.INFO_TRY_AGAIN_LATER
                        || outIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    continue;
                }
                if (outIdx >= 0) {
                    Image img = codec.getOutputImage(outIdx);
                    if (img != null && info.size > 0) {
                        // 编码帧按 mb 对齐（如 886→896），可视区由输出 format 的
                        // crop 矩形给出——只拷可视区，右/下边缘是填充垃圾
                        MediaFormat of = codec.getOutputFormat();
                        int cl = 0, ct = 0, vw = w, vh = h;
                        if (of != null && of.containsKey("crop-left")) {
                            cl = of.getInteger("crop-left");
                            ct = of.getInteger("crop-top");
                            vw = of.getInteger("crop-right") - cl + 1;
                            vh = of.getInteger("crop-bottom") - ct + 1;
                        }
                        if (vw != w || vh != h) {
                            System.out.println("[LayerTest] recon crop " + vw + "x" + vh
                                    + " != " + w + "x" + h);
                            codec.releaseOutputBuffer(outIdx, false);
                            return null;
                        }
                        ByteBuffer recon = ByteBuffer.allocateDirect(w * h * 3 / 2);
                        Image.Plane[] planes = img.getPlanes();
                        Layered.nativeCopyPlanes(
                                sliceOff(planes[0].getBuffer(),
                                        ct * planes[0].getRowStride()
                                                + cl * planes[0].getPixelStride()),
                                planes[0].getRowStride(), planes[0].getPixelStride(),
                                sliceOff(planes[1].getBuffer(),
                                        ct / 2 * planes[1].getRowStride()
                                                + cl / 2 * planes[1].getPixelStride()),
                                planes[1].getRowStride(), planes[1].getPixelStride(),
                                sliceOff(planes[2].getBuffer(),
                                        ct / 2 * planes[2].getRowStride()
                                                + cl / 2 * planes[2].getPixelStride()),
                                planes[2].getRowStride(), planes[2].getPixelStride(),
                                slice0(recon), w, h);
                        codec.releaseOutputBuffer(outIdx, false);
                        long decMs = (System.nanoTime() - t0) / 1_000_000;
                        System.out.println("[LayerTest] decode_one_frame dec_ms=" + decMs);
                        return recon;
                    }
                    codec.releaseOutputBuffer(outIdx, false);
                }
            }
            System.out.println("[LayerTest] decode timeout");
            return null;
        } catch (Exception e) {
            System.out.println("[LayerTest] decode failed: " + e);
            e.printStackTrace(System.out);
            return null;
        } finally {
            if (codec != null) {
                try {
                    codec.stop();
                } catch (Exception ignored) {}
                try {
                    codec.release();
                } catch (Exception ignored) {}
            }
        }
    }

    private static ByteBuffer slice0(ByteBuffer buf) {
        ByteBuffer d = buf.duplicate();
        d.position(0);
        return d.slice();
    }

    /** 从 buffer 数据起点偏移 off 字节处切片（容量 = 剩余，JNI 侧做容量防御）。 */
    private static ByteBuffer sliceOff(ByteBuffer buf, int off) {
        ByteBuffer d = buf.duplicate();
        d.position(0);
        d.limit(d.capacity());
        if (off < 0 || off > d.capacity()) off = 0;
        d.position(off);
        return d.slice();
    }

    public static int run(String rgbaPath, String outDir) {
        byte[] rgba;
        try {
            rgba = Files.readAllBytes(new File(rgbaPath).toPath());
        } catch (IOException e) {
            System.out.println("[PsnrTest] read " + rgbaPath + " failed: " + e);
            return 1;
        }
        if (rgba.length != W * H * 4) {
            System.out.println("[PsnrTest] bad rgba size " + rgba.length
                    + ", want " + (W * H * 4) + " (" + W + "x" + H + ")");
            return 1;
        }
        File dir = new File(outDir);
        if (!dir.isDirectory() && !dir.mkdirs()) {
            System.out.println("[PsnrTest] mkdir " + outDir + " failed");
            return 1;
        }

        // app_process 主线程无 Looper：MediaCodec.configure 需要（同 ScreenEncoder）
        Looper.prepare();

        // 预先各做一次 native 重排（7 路共用输入）
        ByteBuffer singleI420 = ByteBuffer.allocateDirect(W * H * 3 / 2);
        Repack.rgbaToI420Array(rgba, W, H, singleI420);
        ByteBuffer dualI420 = ByteBuffer.allocateDirect(W * 2 * H * 3 / 2);
        Repack.repackArray(rgba, W, H, dualI420);
        ByteBuffer dualYcocg = ByteBuffer.allocateDirect(W * 2 * H * 3 / 2);
        Repack.repackArrayYcocg(rgba, W, H, dualYcocg);
        System.out.println("[PsnrTest] repacked single I420 + dual raw + dual ycocg ("
                + W + "x" + H + ")");

        int rc = 0;
        rc |= encodeOne(singleI420, W, H, 6_000_000, new File(dir, "single_6M.h264"));
        rc |= encodeOne(singleI420, W, H, 3_000_000, new File(dir, "single_3M.h264"));
        rc |= encodeOne(singleI420, W, H, 2_000_000, new File(dir, "single_2M.h264"));
        rc |= encodeOne(singleI420, W, H, 4_000_000, new File(dir, "single_4M.h264"));
        rc |= encodeOne(singleI420, W, H, 5_000_000, new File(dir, "single_5M.h264"));
        rc |= encodeOne(singleI420, W, H, 12_000_000, new File(dir, "single_12M.h264"));
        rc |= encodeOne(singleI420, W, H, 40_000_000, new File(dir, "single_40M.h264"));
        rc |= encodeOne(singleI420, W, H, 30_000_000, new File(dir, "single_30M.h264"));
        rc |= encodeOne(singleI420, W, H, 24_000_000, new File(dir, "single_24M.h264"));
        rc |= encodeOne(singleI420, W, H, 100_000_000, new File(dir, "single_100M.h264"));
        rc |= encodeOne(dualI420, W * 2, H, 6_000_000, new File(dir, "dual_6M.h264"));
        rc |= encodeOne(dualI420, W * 2, H, 9_000_000, new File(dir, "dual_9M.h264"));
        rc |= encodeOne(dualI420, W * 2, H, 12_000_000, new File(dir, "dual_12M.h264"));
        rc |= encodeOne(dualYcocg, W * 2, H, 6_000_000, new File(dir, "dual_ycocg_6M.h264"));
        rc |= encodeOne(dualYcocg, W * 2, H, 5_000_000, new File(dir, "dual_ycocg_5M.h264"));
        rc |= encodeOne(dualYcocg, W * 2, H, 4_000_000, new File(dir, "dual_ycocg_4M.h264"));
        rc |= encodeOne(dualYcocg, W * 2, H, 2_000_000, new File(dir, "dual_ycocg_2M.h264"));
        rc |= encodeOne(dualYcocg, W * 2, H, 1_500_000, new File(dir, "dual_ycocg_1_5M.h264"));
        rc |= encodeOne(dualYcocg, W * 2, H, 2_500_000, new File(dir, "dual_ycocg_2_5M.h264"));
        rc |= encodeOne(dualYcocg, W * 2, H, 9_000_000, new File(dir, "dual_ycocg_9M.h264"));
        rc |= encodeOne(dualYcocg, W * 2, H, 12_000_000, new File(dir, "dual_ycocg_12M.h264"));
        rc |= encodeOne(dualYcocg, W * 2, H, 20_000_000, new File(dir, "dual_ycocg_20M.h264"));
        rc |= encodeOne(dualYcocg, W * 2, H, 40_000_000, new File(dir, "dual_ycocg_40M.h264"));
        rc |= encodeOne(dualYcocg, W * 2, H, 30_000_000, new File(dir, "dual_ycocg_30M.h264"));
        rc |= encodeOne(dualYcocg, W * 2, H, 24_000_000, new File(dir, "dual_ycocg_24M.h264"));
        rc |= encodeOne(dualYcocg, W * 2, H, 100_000_000, new File(dir, "dual_ycocg_100M.h264"));
        System.out.println("[PsnrTest] " + (rc == 0 ? "ALL DONE" : "FAILED rc=" + rc));
        return rc;
    }

    /** 用一份 I420 帧建独立编码器编一帧并冲刷，输出 Annex-B（文件头先写 SPS/PPS）。 */
    private static int encodeOne(ByteBuffer i420, int encW, int encH, int bitrate, File outFile) {
        final int frameBytes = encW * encH * 3 / 2;
        MediaCodec codec = null;
        try (FileOutputStream fos = new FileOutputStream(outFile)) {
            MediaFormat format = MediaFormat.createVideoFormat(MIME, encW, encH);
            format.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                    MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
            format.setInteger(MediaFormat.KEY_BIT_RATE, bitrate);
            format.setInteger(MediaFormat.KEY_FRAME_RATE, FPS);
            format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 10);
            format.setInteger(MediaFormat.KEY_BITRATE_MODE,
                    MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR);
            format.setInteger("latency", 0);   // KEY_LATENCY，与生产一致
            format.setInteger("priority", 0);  // KEY_PRIORITY：实时优先

            codec = MediaCodec.createEncoderByType(MIME);
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            codec.start();

            // 喂唯一一帧
            int inIdx = codec.dequeueInputBuffer(TIMEOUT_US);
            if (inIdx < 0) throw new IllegalStateException("no input buffer for frame");
            Image input = codec.getInputImage(inIdx);
            if (input == null) throw new IllegalStateException("getInputImage null");
            Repack.fillInputImage(i420, encW, encH, input);
            codec.queueInputBuffer(inIdx, 0, frameBytes, 0, 0);

            // 紧跟 EOS 冲刷
            inIdx = codec.dequeueInputBuffer(TIMEOUT_US);
            if (inIdx < 0) throw new IllegalStateException("no input buffer for EOS");
            codec.queueInputBuffer(inIdx, 0, 0, 1, MediaCodec.BUFFER_FLAG_END_OF_STREAM);

            // 排干输出：SPS/PPS 先写入文件头，再写 IDR（CODEC_CONFIG 缓冲兜底）
            boolean csdWritten = false;
            boolean eos = false;
            long total = 0;
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            while (!eos) {
                int outIdx = codec.dequeueOutputBuffer(info, TIMEOUT_US);
                if (outIdx == MediaCodec.INFO_TRY_AGAIN_LATER) {
                    continue;
                } else if (outIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    MediaFormat f = codec.getOutputFormat();
                    ByteBuffer c0 = f.getByteBuffer("csd-0");
                    ByteBuffer c1 = f.getByteBuffer("csd-1");
                    if (c0 != null && c1 != null) {
                        byte[] sps = toArray(c0);
                        byte[] pps = toArray(c1);
                        fos.write(sps);
                        fos.write(pps);
                        csdWritten = true;
                        System.out.println("[PsnrTest] " + outFile.getName()
                                + " csd-0=" + sps.length + "B csd-1=" + pps.length + "B");
                    }
                } else if (outIdx >= 0) {
                    ByteBuffer buf = codec.getOutputBuffer(outIdx);
                    if (buf != null && info.size > 0) {
                        if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
                            if (!csdWritten) {  // 编码器不走 FORMAT_CHANGED 时的兜底
                                fos.write(toArray(buf, info));
                                csdWritten = true;
                            }
                        } else {
                            ByteBuffer dup = buf.duplicate();
                            dup.position(info.offset);
                            dup.limit(info.offset + info.size);
                            byte[] frame = new byte[info.size];
                            dup.get(frame);
                            fos.write(frame);
                            total += info.size;
                            System.out.println("[PsnrTest] " + outFile.getName()
                                    + " frame " + info.size + "B flags=0x"
                                    + Integer.toHexString(info.flags));
                        }
                    }
                    codec.releaseOutputBuffer(outIdx, false);
                    eos = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                }
            }
            System.out.println("[PsnrTest] wrote " + outFile.getAbsolutePath()
                    + " payload=" + total + "B encW=" + encW + " bitrate=" + bitrate
                    + " csdWritten=" + csdWritten);
            return 0;
        } catch (Exception e) {
            System.out.println("[PsnrTest] encode " + outFile.getName() + " failed: " + e);
            e.printStackTrace(System.out);
            return 1;
        } finally {
            if (codec != null) {
                try {
                    codec.stop();
                } catch (Exception ignored) {}
                try {
                    codec.release();
                } catch (Exception ignored) {}
            }
        }
    }

    private static byte[] toArray(ByteBuffer buf) {
        ByteBuffer dup = buf.duplicate();
        byte[] arr = new byte[dup.remaining()];
        dup.get(arr);
        return arr;
    }

    private static byte[] toArray(ByteBuffer buf, MediaCodec.BufferInfo info) {
        ByteBuffer dup = buf.duplicate();
        dup.position(info.offset);
        dup.limit(info.offset + info.size);
        byte[] arr = new byte[info.size];
        dup.get(arr);
        return arr;
    }
}
