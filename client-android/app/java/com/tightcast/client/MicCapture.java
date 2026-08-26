package com.tightcast.client;

import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;

/**
 * 麦克风采集（协议 §4 声音注入）：AudioRecord 16kHz mono PCM16，
 * 40ms（640 采样 = 1280B）一帧转发 native → tight 通道 1 → server 手机扬声器。
 * 与 Windows 端 waveIn 路径等价。
 */
public final class MicCapture {

    private static final int SAMPLE_RATE = 16000;
    private static final int FRAME_SAMPLES = 640;  // 40ms

    private volatile boolean running;
    private Thread thread;
    private AudioRecord record;

    public synchronized void start() {
        if (running) return;
        int minBuf = AudioRecord.getMinBufferSize(SAMPLE_RATE,
                AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
        if (minBuf <= 0) {
            System.out.println("[MicCapture] getMinBufferSize failed: " + minBuf);
            return;
        }
        try {
            record = new AudioRecord(MediaRecorder.AudioSource.MIC, SAMPLE_RATE,
                    AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT,
                    Math.max(minBuf, FRAME_SAMPLES * 2 * 4));
        } catch (Exception e) {
            System.out.println("[MicCapture] create failed: " + e);
            record = null;
            return;
        }
        running = true;
        thread = new Thread(this::loop, "mic-capture");
        thread.start();
        System.out.println("[MicCapture] started");
    }

    public synchronized void stop() {
        running = false;
        if (thread != null) {
            try {
                thread.join(1000);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
            thread = null;
        }
    }

    private void loop() {
        AudioRecord r = record;
        if (r == null) return;
        try {
            r.startRecording();
        } catch (Exception e) {
            System.out.println("[MicCapture] startRecording failed: " + e);
            return;
        }
        byte[] buf = new byte[FRAME_SAMPLES * 2];
        while (running) {
            int n = r.read(buf, 0, buf.length);
            if (n > 0) {
                NativeClient.nativePcm(buf, n);
            } else if (n < 0) {
                System.out.println("[MicCapture] read error: " + n);
                break;
            }
        }
        try {
            r.stop();
        } catch (Exception ignored) {}
        r.release();
        System.out.println("[MicCapture] stopped");
    }
}
