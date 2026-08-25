package com.tightcast.server;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;

/**
 * 声音注入：AudioTrack(STREAM_MUSIC, 16kHz, mono, PCM_16BIT) 播放 client 推来的 PCM。
 * 延迟创建（首块音频到达时才建），MODE_STREAM 收到即写。
 */
public final class AudioInjector {

    private static final int SAMPLE_RATE = 16000;
    private static final int FRAME_BYTES = 1280; // 40ms 640 采样 s16le
    private static final int BUFFER_BYTES = FRAME_BYTES * 4;

    private AudioTrack track;

    public synchronized void write(byte[] pcm) {
        write(pcm, 0, pcm != null ? pcm.length : 0);
    }

    public synchronized void write(byte[] pcm, int offset, int length) {
        if (pcm == null || length <= 0) return;
        if (track == null) {
            try {
                int min = AudioTrack.getMinBufferSize(SAMPLE_RATE,
                        AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT);
                track = new AudioTrack.Builder()
                        .setAudioAttributes(new AudioAttributes.Builder()
                                .setUsage(AudioAttributes.USAGE_MEDIA)
                                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                                .build())
                        .setAudioFormat(new AudioFormat.Builder()
                                .setSampleRate(SAMPLE_RATE)
                                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                                .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                                .build())
                        .setBufferSizeInBytes(Math.max(min, BUFFER_BYTES))
                        .setTransferMode(AudioTrack.MODE_STREAM)
                        .build();
                track.play();
                System.out.println("[AudioInjector] AudioTrack started, buffer="
                        + Math.max(min, BUFFER_BYTES) + "B");
            } catch (Exception e) {
                System.out.println("[AudioInjector] init failed: " + e);
                track = null;
                return;
            }
        }
        try {
            track.write(pcm, offset, length);
        } catch (Exception e) {
            System.out.println("[AudioInjector] write failed: " + e);
        }
    }

    public synchronized void release() {
        if (track != null) {
            try {
                track.stop();
            } catch (Exception ignored) {}
            track.release();
            track = null;
        }
    }
}
