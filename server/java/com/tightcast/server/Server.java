package com.tightcast.server;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;

/**
 * main 入口：解析参数、装配各模块、启动 tight server。
 *
 * 用法：app_process /system/bin com.tightcast.server.Server \
 *          --port 8800 --token tightcast --bitrate 6000000 --fps 30 --max-size 1920 \
 *          [--no-audio] [--gl-repack] [--repack-selftest]
 *
 * 重排路径：默认 CPU（实测 SD865 上 ~7.5ms/帧，快于 GL 的 ~10ms——瓶颈在
 * GPU→CPU 回读带宽）；--gl-repack 强制走 GPU shader 重排（省 CPU 但慢）。
 */
public final class Server {

    private Server() {}

    public static void main(String[] args) {
        Thread.setDefaultUncaughtExceptionHandler((t, e) -> {
            System.out.println("[Server] uncaught exception in " + t.getName() + ": " + e);
            e.printStackTrace(System.out);
        });

        int port = 8800;
        String token = "tightcast";
        int bitrate = 6_000_000;
        int fps = 30;
        int maxSize = 1920;  // 1080p：长边 1920（如 1080x2340 屏 → 883x1920）
        boolean audio = true;
        boolean glRepack = false;
        // 视频格式模式（协议 §5 命令 0x06 位域）：默认 double-ycocg
        int formatMode = ScreenEncoder.MODE_COLOR_YCOCG;
        double baseShare = 0.35;   // layer 模式基础层码率占比
        int enhImpl = ScreenEncoder.ENH_H264;  // 增强层实现：H.264 直偏置硬编（默认）
        boolean selftest = false;
        String psnrRgba = null;   // --psnr-test <rgba_path> <out_dir>
        String psnrOutDir = null;
        String layerRgba = null;  // --layer-test <rgba_path> <out_dir>（方案A 实验）
        String layerOutDir = null;
        for (int i = 0; i < args.length; i++) {
            try {
                switch (args[i]) {
                    case "--port":
                        port = Integer.parseInt(args[++i]);
                        break;
                    case "--token":
                        token = args[++i];
                        break;
                    case "--bitrate":
                        bitrate = Integer.parseInt(args[++i]);
                        break;
                    case "--fps":
                        fps = Integer.parseInt(args[++i]);
                        break;
                    case "--max-size":
                        maxSize = Integer.parseInt(args[++i]);
                        break;
                    case "--no-audio":
                        audio = false;
                        break;
                    case "--gl-repack":
                        glRepack = true;
                        break;
                    case "--yuv-raw":
                        formatMode = 0;  // 回退协议 §3.1 的 RGB 原样搬运（double-raw）
                        break;
                    case "--mode":
                        switch (args[++i]) {
                            case "double-raw":
                                formatMode = 0;
                                break;
                            case "double-ycocg":
                                formatMode = ScreenEncoder.MODE_COLOR_YCOCG;
                                break;
                            case "single":
                                formatMode = ScreenEncoder.MODE_GEOM_SINGLE;
                                break;
                            case "layer":
                                formatMode = ScreenEncoder.MODE_GEOM_SINGLE
                                        | ScreenEncoder.MODE_LAYERED;
                                break;
                            default:
                                System.out.println("[Server] unknown --mode: " + args[i]);
                                System.exit(1);
                        }
                        break;
                    case "--base-share":
                        baseShare = Double.parseDouble(args[++i]);
                        break;
                    case "--enh-impl":
                        enhImpl = "rice".equals(args[++i])
                                ? ScreenEncoder.ENH_RICE : ScreenEncoder.ENH_H264;
                        break;
                    case "--repack-selftest":
                        selftest = true;
                        break;
                    case "--psnr-test":
                        psnrRgba = args[++i];
                        psnrOutDir = args[++i];
                        break;
                    case "--layer-test":
                        layerRgba = args[++i];
                        layerOutDir = args[++i];
                        break;
                    default:
                        System.out.println("[Server] ignoring unknown arg: " + args[i]);
                }
            } catch (Exception e) {
                System.out.println("[Server] bad arg " + args[i] + ": " + e);
                System.exit(1);
            }
        }
        System.out.println("[Server] port=" + port + " bitrate=" + bitrate + " fps=" + fps
                + " maxSize=" + maxSize + " audio=" + audio + " glRepack=" + glRepack
                + " mode=0x" + Integer.toHexString(formatMode) + " baseShare=" + baseShare);

        System.load("/data/local/tmp/tightcast/libtight_jni.so");
        System.out.println("[Server] libtight_jni loaded");

        // PSNR 对比测试：编 4 路 h264 后退出（不启动 tight server）
        if (psnrRgba != null) {
            System.exit(PsnrTest.run(psnrRgba, psnrOutDir));
        }

        // 方案A 实验：残差直偏置 vs 差分伪装硬件编码对比（编完退出）
        if (layerRgba != null) {
            System.exit(PsnrTest.runLayerTest(layerRgba, layerOutDir));
        }

        // 重排自检：已知图案分别走 CPU / GL 两路径，逐字节比对后退出
        if (selftest) {
            boolean ok = GlRepack.selfTest(64, 48) && GlRepack.selfTest(886, 1920);
            System.out.println("[Server] repack selftest " + (ok ? "PASS" : "FAIL"));
            return;
        }

        final ScreenEncoder encoder = new ScreenEncoder(bitrate, fps, maxSize, glRepack,
                formatMode, baseShare, enhImpl);
        final ControlInjector control = new ControlInjector(encoder);
        final AudioInjector audioInjector = audio ? new AudioInjector() : null;

        TightBridge.nativeSetListener(new TightBridge.Listener() {
            @Override
            public void onAudio(byte[] pcm) {
                // 首字节 tag=0xA1（避开 tight 内部保留首字节），剥掉后送 AudioTrack
                if (audioInjector != null && pcm != null && pcm.length > 1 && pcm[0] == (byte) 0xA1) {
                    audioInjector.write(pcm, 1, pcm.length - 1);
                }
            }

            @Override
            public void onCommand(byte[] cmd) {
                control.handle(cmd);
            }

            @Override
            public void onPeerState(String peerId, boolean online) {
                System.out.println("[Server] peer " + peerId + (online ? " online" : " closed"));
                if (online) {
                    sendDeviceInfo(encoder);
                    encoder.requestKeyframe();
                    encoder.requestEnhKeyframe();  // 增强链同样以 IDR 起链（ch4）
                } else {
                    // 内核把 tight 的 UDP socket connect 到旧对端后，来自新客户端的
                    // 报文被 NoPorts 丢弃。进程内 nativeStop 重建会挂死（stop 需 join
                    // 可能卡在 Java 回调里的 tight 线程）——直接退出进程，由外层
                    // 监督循环（run.sh 的 while true）拉起来获得全新 socket。
                    System.out.println("[Server] last peer closed, exit for respawn");
                    System.exit(0);
                }
            }

            @Override
            public void onVideoCapacity(long bps) {
                encoder.setBitrate(bps);
            }

            @Override
            public void onRequestKeyframe() {
                encoder.requestKeyframe();
            }
        });

        if (!TightBridge.nativeStart(port, token)) {
            System.out.println("[Server] nativeStart failed");
            System.exit(1);
        }
        System.out.println("[Server] tight listening on 0.0.0.0:" + port);

        encoder.start();
        // 旋转重建后重发 DEVICE_INFO（尺寸可能变化）
        encoder.setOnDisplaySizeChanged(() -> sendDeviceInfo(encoder));

        // 诊断：每 2s 打 tight 发送侧统计
        Thread stats = new Thread(() -> {
            while (true) {
                try {
                    Thread.sleep(2000);
                } catch (InterruptedException e) {
                    return;
                }
                long[] s = TightBridge.nativeStats();
                if (s != null) {
                    System.out.println("[stats] est=" + s[0] + " btl=" + s[1] + " cap=" + s[2]
                            + " outq=" + s[3] + " vok=" + s[4] + " vfail=" + s[5]
                            + " " + TightBridge.nativePeers());
                }
            }
        });
        stats.setDaemon(true);
        stats.start();

        // 主线程阻塞
        try {
            Thread.sleep(Long.MAX_VALUE);
        } catch (InterruptedException ignored) {
        }
    }

    /** DEVICE_INFO（协议第 6 节）：0x01 + u16 w + u16 h + name_len + UTF-8 型号。 */
    private static void sendDeviceInfo(ScreenEncoder encoder) {
        int w = encoder.getVideoWidth();
        int h = encoder.getVideoHeight();
        if (w <= 0 || h <= 0) return; // 编码器尚未初始化
        byte[] name = DeviceInfo.model().getBytes(StandardCharsets.UTF_8);
        ByteBuffer buf = ByteBuffer.allocate(6 + name.length).order(ByteOrder.BIG_ENDIAN);
        buf.put((byte) 0x01);
        buf.putShort((short) w);
        buf.putShort((short) h);
        buf.put((byte) name.length);
        buf.put(name);
        boolean ok = TightBridge.nativeSendData(buf.array());
        System.out.println("[Server] DEVICE_INFO " + w + "x" + h + " sent=" + ok);
    }
}
