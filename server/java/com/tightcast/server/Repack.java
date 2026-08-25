package com.tightcast.server;

import android.media.Image;

import java.nio.ByteBuffer;

/**
 * RGBA_8888 → 双 YUV420 左右拼接（协议 §3.1）的 native 封装。
 * 实现位于 server/jni/repack.cpp（核心逻辑 repack_core.cpp，host 端有单测）。
 * native 侧用 GetDirectBufferAddress 零拷贝读取，故传入的 buffer 一律先 slice()
 * 到 position=0，保证地址语义一致。
 */
public final class Repack {

    private Repack() {}

    /**
     * ImageReader 的 RGBA_8888 帧重排为 2W×H I420，写入 dst（direct，容量 ≥ 2W*H*3/2）。
     * ycocg=true 走协议 §3.2（YCoCg 色差打包，默认）；false 走 §3.1（RGB 原样搬运）。
     * 自动处理 plane 的 rowStride/pixelStride；非 direct buffer 走数组回退。
     */
    public static void repack(Image image, int w, int h, ByteBuffer dst, boolean ycocg) {
        Image.Plane p = image.getPlanes()[0];
        ByteBuffer buf = p.getBuffer();
        int rowStride = p.getRowStride();
        int pixelStride = p.getPixelStride();
        dst.clear();
        if (buf.isDirect()) {
            nativeRepackDirect(slice0(buf), w, h, rowStride, pixelStride, slice0(dst), ycocg);
        } else if (buf.hasArray()) {
            if (ycocg) {
                // 数组回退只有 raw 实现：ycocg 下不应走到（ImageReader 平面必为 direct）
                throw new IllegalStateException("ycocg repack requires direct buffer");
            }
            nativeRepackArray(buf.array(), buf.arrayOffset() + buf.position(),
                    w, h, rowStride, pixelStride, slice0(dst));
        } else {
            throw new IllegalStateException("ImageReader plane buffer neither direct nor array");
        }
    }

    /**
     * 把 2W×H I420（repack 输出）填入编码器输入 Image 的各平面，
     * 按各 plane 的 rowStride/pixelStride 拷贝，兼容 planar 三平面与 NV12 交错两平面。
     */
    public static void fillInputImage(ByteBuffer srcI420, int encW, int encH, Image input) {
        Image.Plane[] planes = input.getPlanes();
        nativeFillInputImage(slice0(srcI420), encW, encH,
                slice0(planes[0].getBuffer()), planes[0].getRowStride(), planes[0].getPixelStride(),
                slice0(planes[1].getBuffer()), planes[1].getRowStride(), planes[1].getPixelStride(),
                slice0(planes[2].getBuffer()), planes[2].getRowStride(), planes[2].getPixelStride());
    }

    /** 自检用：RGBA byte 数组（紧凑 W*4 行距、pixelStride=4）→ 2W×H I420。 */
    static void repackArray(byte[] rgba, int w, int h, ByteBuffer dst) {
        dst.clear();
        nativeRepackArray(rgba, 0, w, h, w * 4, 4, slice0(dst));
    }

    /** PSNR 测试用：RGBA byte 数组 → 2W×H I420（YCoCg 打包，协议 §3.2）。 */
    static void repackArrayYcocg(byte[] rgba, int w, int h, ByteBuffer dst) {
        dst.clear();
        nativeRepackArrayYcocg(rgba, 0, w, h, w * 4, 4, slice0(dst));
    }

    /** 单 YUV420 对照路径（PSNR 测试）：RGBA byte 数组 → BT.601 I420（W×H）。 */
    public static void rgbaToI420Array(byte[] rgba, int w, int h, ByteBuffer dst) {
        dst.clear();
        nativeRgbaToI420(rgba, 0, w, h, w * 4, 4, slice0(dst));
    }

    private static ByteBuffer slice0(ByteBuffer buf) {
        ByteBuffer dup = buf.duplicate();
        dup.position(0);
        return dup.slice();
    }

    private static native void nativeRepackDirect(ByteBuffer rgba, int w, int h,
            int rowStride, int pixelStride, ByteBuffer dst, boolean ycocg);

    private static native void nativeRepackArray(byte[] rgba, int offset, int w, int h,
            int rowStride, int pixelStride, ByteBuffer dst);

    private static native void nativeRepackArrayYcocg(byte[] rgba, int offset, int w, int h,
            int rowStride, int pixelStride, ByteBuffer dst);

    private static native void nativeRgbaToI420(byte[] rgba, int offset, int w, int h,
            int rowStride, int pixelStride, ByteBuffer dst);

    private static native void nativeFillInputImage(ByteBuffer src, int encW, int encH,
            ByteBuffer yBuf, int yRowStride, int yPixelStride,
            ByteBuffer uBuf, int uRowStride, int uPixelStride,
            ByteBuffer vBuf, int vRowStride, int vPixelStride);
}
