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
        rc |= encodeOne(singleI420, W, H, 12_000_000, new File(dir, "single_12M.h264"));
        rc |= encodeOne(singleI420, W, H, 40_000_000, new File(dir, "single_40M.h264"));
        rc |= encodeOne(dualI420, W * 2, H, 6_000_000, new File(dir, "dual_6M.h264"));
        rc |= encodeOne(dualI420, W * 2, H, 9_000_000, new File(dir, "dual_9M.h264"));
        rc |= encodeOne(dualI420, W * 2, H, 12_000_000, new File(dir, "dual_12M.h264"));
        rc |= encodeOne(dualYcocg, W * 2, H, 6_000_000, new File(dir, "dual_ycocg_6M.h264"));
        rc |= encodeOne(dualYcocg, W * 2, H, 9_000_000, new File(dir, "dual_ycocg_9M.h264"));
        rc |= encodeOne(dualYcocg, W * 2, H, 12_000_000, new File(dir, "dual_ycocg_12M.h264"));
        rc |= encodeOne(dualYcocg, W * 2, H, 20_000_000, new File(dir, "dual_ycocg_20M.h264"));
        rc |= encodeOne(dualYcocg, W * 2, H, 40_000_000, new File(dir, "dual_ycocg_40M.h264"));
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
