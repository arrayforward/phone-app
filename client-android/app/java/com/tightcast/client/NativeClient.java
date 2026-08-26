package com.tightcast.client;

import java.nio.ByteBuffer;

/**
 * C++ 核心（libtightcast_client.so）的 JNI 声明。
 * GL 三个回调由 GLSurfaceView 渲染线程调用；其余任意线程。
 */
public final class NativeClient {

    static {
        System.loadLibrary("tightcast_client");
    }

    private NativeClient() {}

    public static native boolean nativeStart(String host, int port, String token);
    public static native void nativeStop();
    public static native boolean nativeIsOnline();

    /** SET_FORMAT（协议 §5 命令 0x06）：mode 位域 bit0=single bit1=ycocg bit2=layer。 */
    public static native void nativeSetMode(int mode);

    /** 上屏 grace 窗口（ms）：等增强层赶上合成；0=立即上屏基础帧。默认 150。 */
    public static native void nativeSetEnhWait(int ms);

    /** 触控：action 0=DOWN 1=UP 2=MOVE；x/y 归一化（相对视频画面）。 */
    public static native void nativeTouch(int action, float x, float y);
    /** 按键：action 0=DOWN 1=UP；keycode 为 Android KeyEvent 键码（直通注入）。 */
    public static native void nativeKey(int action, int keycode);
    public static native void nativeText(String text);
    public static native void nativeScroll(float x, float y, float dy);
    /** 麦克风 PCM（16kHz mono s16le）。 */
    public static native void nativePcm(byte[] pcm, int len);

    /** 逻辑视频尺寸 [w, h]；未知返回 [0, 0]。 */
    public static native int[] nativeVideoSize();

    /** 上屏统计行："shown=N composited=M enh_idr=0/1"（合成命中率观测）。 */
    public static native String nativeStatsLine();

    /** 注册 Java 解码器（VideoDec，layer 0=基础 1=增强）。华为等 NDK
     *  AImageReader 输出面不通的机型上的主解码路径。 */
    public static native void nativeSetVideoDecs(VideoDec base, VideoDec enh);

    /** VideoDec 解码输出回调（codec 线程 → native 排产）。 */
    static native void nativeOnDecodedFrame(int layer, long ptsMs, int flags,
            ByteBuffer y, int yRowStride, int yPixelStride,
            ByteBuffer u, int uRowStride, int uPixelStride,
            ByteBuffer v, int vRowStride, int vPixelStride,
            int w, int h);

    public static native void nativeSurfaceCreated();
    public static native void nativeSurfaceChanged(int w, int h);
    public static native void nativeDrawFrame();
}
