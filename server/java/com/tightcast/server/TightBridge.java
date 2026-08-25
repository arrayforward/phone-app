package com.tightcast.server;

/**
 * tight 传输层 JNI 封装。native 侧为 LinkRole::Node，bind 0.0.0.0:&lt;port&gt;。
 * Listener 回调均从 native 线程触发，实现里只做排队/置标志，须快速返回。
 */
public final class TightBridge {

    public interface Listener {
        /** client 推来的通道 1 音频 PCM（16kHz mono s16le，40ms/块）。 */
        void onAudio(byte[] pcm);
        /** command 通道命令（协议第 5 节）。 */
        void onCommand(byte[] cmd);
        /** peer 上线/下线。 */
        void onPeerState(String peerId, boolean online);
        /** 视频可用码率（bps），据此调整编码码率。 */
        void onVideoCapacity(long bps);
        /** evac_keyframe / loan_exhausted(true) → 强制出新 IDR。 */
        void onRequestKeyframe();
    }

    private TightBridge() {}

    public static native boolean nativeStart(int port, String token);

    public static native void nativeStop();

    /** 发送视频消息（通道 0）给当前唯一在线 peer。 */
    public static native boolean nativeSendVideo(byte[] frame, boolean keyframe);

    /** 发送可靠数据消息（通道 3 data）。 */
    public static native boolean nativeSendData(byte[] payload);

    /** 诊断：[est_bps, btl_bps, video_capacity_bps, outbound_queue, send_ok, send_fail]。 */
    public static native long[] nativeStats();

    /** 诊断：当前发送目标与全部 peer 状态（"chosen=X peers=[id:state ...]"）。 */
    public static native String nativePeers();

    public static native void nativeSetListener(Listener listener);
}
