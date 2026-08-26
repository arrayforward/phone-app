package com.tightcast.server;

import java.nio.ByteBuffer;

/**
 * 增强层残差熵编码（协议 docs/protocol.md §3.4）的 native 封装。
 * 实现位于 server/jni/layered_jni.cpp（核心逻辑 shared/layered，host 端有单测）。
 */
public final class Layered {

    private Layered() {}

    /**
     * 解码输出 Image 三平面 → 紧凑 planar I420 拷贝（dst direct，容量 ≥ W*H*3/2）。
     * 在 BaseDecoder 回调线程上执行（拷完即释放解码输出缓冲）。
     */
    public static native void nativeCopyPlanes(
            ByteBuffer ry, int yRowStride, int yPixelStride,
            ByteBuffer ru, int uRowStride, int uPixelStride,
            ByteBuffer rv, int vRowStride, int vPixelStride,
            ByteBuffer dst, int w, int h);

    /**
     * 残差 sym 平面原样输出（--layer-test 实验用）：clamp(orig−recon,−128,127)+128，
     * 紧凑 planar（Y W×H → U → V），128 = 零残差。
     */
    public static native byte[] nativeComputeSym(ByteBuffer orig, int w, int h,
            ByteBuffer ry, int yRowStride, int yPixelStride,
            ByteBuffer ru, int uRowStride, int uPixelStride,
            ByteBuffer rv, int vRowStride, int vPixelStride);

    /**
     * 迷彩变换（方案A）：inverse=false 逐行模 256 一阶差分；true 前缀和逆变换。
     * sym 为紧凑 planar（W*H*3/2）。往返逐字节无损。
     */
    public static native byte[] nativeCamouflage(byte[] sym, int w, int h, boolean inverse);

    /**
     * 计算一帧的增强层残差并熵编码。
     *
     * @param orig  原始 I420 帧（direct，紧凑 planar，W*H*3/2，position=0 slice）
     * @param w/h   逻辑帧尺寸（single 几何 = 编码帧尺寸）
     * @param ry/ru/rv  基础层重建帧（MediaCodec 解码输出 Image 的三平面 buffer，
     *                  各带 rowStride/pixelStride；pixelStride=2 为 NV12 半平面交错）
     * @param maxBytes  尺寸护栏（≤0 取默认 512KB）
     * @return 残差熵编码载荷（不含 9 字节消息头）；护栏触发/异常返回 null
     */
    public static native byte[] nativeComputeEnhancement(ByteBuffer orig, int w, int h,
            ByteBuffer ry, int yRowStride, int yPixelStride,
            ByteBuffer ru, int uRowStride, int uPixelStride,
            ByteBuffer rv, int vRowStride, int vPixelStride, long maxBytes);
}
