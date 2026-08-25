#pragma once
// waveIn 麦克风采集：16000Hz mono s16le，40ms（1280B）一块，
// 四缓冲轮转，回调即发（protocol.md 第 4 节）。

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

class AudioCapture {
public:
    using Callback = std::function<void(const std::uint8_t* data, std::size_t size)>;

    AudioCapture() = default;
    ~AudioCapture();

    // 失败返回 false（打警告，不影响视频）
    bool start(Callback cb);
    void stop();

    static constexpr int kSampleRate = 16000;
    static constexpr int kBlockBytes = 1280;  // 40ms × 640 采样 × 2B
    static constexpr int kNumBuffers = 4;

private:
    static void CALLBACK wave_in_proc(HWAVEIN, UINT msg, DWORD_PTR instance,
                                      DWORD_PTR p1, DWORD_PTR p2);

    HWAVEIN m_hwi = nullptr;
    Callback m_cb;
    std::atomic<bool> m_running{false};
    void* m_headers = nullptr;  // WAVEHDR[kNumBuffers]（避免头文件引入 mmsystem）
};
