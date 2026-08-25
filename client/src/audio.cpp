#include "audio.h"

#include <windows.h>
#include <mmsystem.h>

#include <cstdio>
#include <cstring>

AudioCapture::~AudioCapture() { stop(); }

bool AudioCapture::start(Callback cb) {
    if (m_running) return true;
    m_cb = std::move(cb);

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = kSampleRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 2;
    wfx.nAvgBytesPerSec = kSampleRate * 2;

    MMRESULT mr = waveInOpen(&m_hwi, WAVE_MAPPER, &wfx,
                             (DWORD_PTR)&AudioCapture::wave_in_proc,
                             (DWORD_PTR)this, CALLBACK_FUNCTION);
    if (mr != MMSYSERR_NOERROR) {
        std::fprintf(stderr, "[audio] waveInOpen failed (%u), audio injection disabled\n",
                     (unsigned)mr);
        m_hwi = nullptr;
        return false;
    }

    auto* hdrs = new WAVEHDR[kNumBuffers]{};
    m_headers = hdrs;
    for (int i = 0; i < kNumBuffers; ++i) {
        hdrs[i].lpData = new CHAR[kBlockBytes];
        hdrs[i].dwBufferLength = kBlockBytes;
        if (waveInPrepareHeader(m_hwi, &hdrs[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR ||
            waveInAddBuffer(m_hwi, &hdrs[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            std::fprintf(stderr, "[audio] prepare/add buffer failed\n");
            stop();
            return false;
        }
    }

    m_running = true;
    if (waveInStart(m_hwi) != MMSYSERR_NOERROR) {
        std::fprintf(stderr, "[audio] waveInStart failed\n");
        stop();
        return false;
    }
    std::fprintf(stderr, "[audio] capture started: 16kHz mono s16, 40ms blocks\n");
    return true;
}

void AudioCapture::stop() {
    m_running = false;
    if (m_hwi) {
        waveInStop(m_hwi);
        waveInReset(m_hwi);  // 挂起的缓冲会触发一次 WIM_DATA（m_running 已 false）
        auto* hdrs = (WAVEHDR*)m_headers;
        for (int i = 0; hdrs && i < kNumBuffers; ++i) {
            waveInUnprepareHeader(m_hwi, &hdrs[i], sizeof(WAVEHDR));
            delete[] hdrs[i].lpData;
        }
        waveInClose(m_hwi);
        m_hwi = nullptr;
    }
    delete[] (WAVEHDR*)m_headers;
    m_headers = nullptr;
}

void CALLBACK AudioCapture::wave_in_proc(HWAVEIN hwi, UINT msg, DWORD_PTR instance,
                                         DWORD_PTR p1, DWORD_PTR) {
    auto* self = (AudioCapture*)instance;
    if (!self || msg != WIM_DATA) return;
    auto* hdr = (WAVEHDR*)p1;
    if (self->m_running && self->m_cb && hdr->dwBytesRecorded > 0) {
        self->m_cb((const std::uint8_t*)hdr->lpData, hdr->dwBytesRecorded);
    }
    if (self->m_running) {
        hdr->dwBytesRecorded = 0;
        waveInAddBuffer(hwi, hdr, sizeof(WAVEHDR));
    }
}
