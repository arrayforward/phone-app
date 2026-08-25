#include "decoder.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <codecapi.h>  // CODECAPI_AVLowLatencyMode
#include <strmif.h>    // ICodecAPI
#include <d3d11.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

// MinGW 头未定义 MFVideoFormat_AVC1：FOURCC 'AVC1' 的媒体类型 GUID
// （值与 Windows SDK 的 DEFINE_MEDIATYPE_GUID(MFVideoFormat_AVC1, FCC('AVC1')) 一致）
const GUID kMFVideoFormat_AVC1 =
    { 0x31435641, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

// MinGW 的 uuid/strmiids 库缺 IID_ICodecAPI：本地定义（值与 Windows SDK 一致）
static const GUID kIID_ICodecAPI =
    { 0x901DB4C7, 0x31CE, 0x41A2, { 0x85, 0xDC, 0x8F, 0xA0, 0xBF, 0x41, 0xB8, 0xDA } };

// 解码器零缓冲低延迟模式（远程操控场景：不攒帧，收到即解即显）
void enable_low_latency(IMFTransform* mft) {
    ICodecAPI* api = nullptr;
    // 用显式 IID：MinGW 下 IID_PPV_ARGS 的 __mingw_uuidof<ICodecAPI> 无定义
    if (FAILED(mft->QueryInterface(kIID_ICodecAPI, reinterpret_cast<void**>(&api))) || !api)
        return;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = 1;  // eAVLowLatencyMode on
    api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
    VariantClear(&v);
    api->Release();
}

// NV12 平面按 stride 原样拷贝为紧凑帧（Y: w×h；UV: w×(h/2) 交错）。
// 不做任何色彩空间变换——打包格式（protocol §3.1）由 GL shader 还原。
void copy_nv12(const std::uint8_t* y_plane, LONG stride, int w, int h, VideoFrame& out) {
    out.width = w;
    out.height = h;
    out.y.resize(static_cast<size_t>(w) * h);
    out.uv.resize(static_cast<size_t>(w) * (h / 2));
    const std::uint8_t* uv_plane = y_plane + static_cast<ptrdiff_t>(stride) * h;
    for (int r = 0; r < h; ++r)
        std::memcpy(out.y.data() + static_cast<size_t>(r) * w,
                    y_plane + static_cast<ptrdiff_t>(r) * stride,
                    static_cast<size_t>(w));
    for (int r = 0; r < h / 2; ++r)
        std::memcpy(out.uv.data() + static_cast<size_t>(r) * w,
                    uv_plane + static_cast<ptrdiff_t>(r) * stride,
                    static_cast<size_t>(w));
}

} // namespace

struct H264Decoder::Impl {
    IMFTransform* mft = nullptr;
    IMFMediaEventGenerator* event_gen = nullptr;  // 异步 MFT（QSV）事件源
    bool async_mode = false;
    DWORD in_id = 0, out_id = 0;
    GUID out_subtype = GUID_NULL;
    int out_w = 0, out_h = 0;
    bool mf_started = false;
    bool com_inited = false;

    // D3D11 硬件加速（MS H.264 MFT + DXGI device manager → GPU 解码）。
    // 输出为 D3D11 NV12 纹理，经 staging 纹理回读后按 stride 原样拷贝平面。
    ID3D11Device* d3d_dev = nullptr;
    ID3D11DeviceContext* d3d_ctx = nullptr;
    IMFDXGIDeviceManager* dxgi_mgr = nullptr;
    UINT dxgi_token = 0;
    ID3D11Texture2D* staging = nullptr;
    int staging_w = 0, staging_h = 0;
    bool use_d3d = false;

    // 给 MFT 挂 D3D11 设备管理器；失败回退纯软解（use_d3d=false）
    bool init_d3d() {
        D3D_FEATURE_LEVEL lvls[2] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL got;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       D3D11_CREATE_DEVICE_VIDEO_SUPPORT |
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       lvls, 2, D3D11_SDK_VERSION,
                                       &d3d_dev, &got, &d3d_ctx);
        if (FAILED(hr) || !d3d_dev) return false;
        hr = MFCreateDXGIDeviceManager(&dxgi_token, &dxgi_mgr);
        if (FAILED(hr)) return false;
        hr = dxgi_mgr->ResetDevice(d3d_dev, dxgi_token);
        if (FAILED(hr)) return false;
        hr = mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                 (ULONG_PTR)static_cast<IMFDXGIDeviceManager*>(dxgi_mgr));
        if (FAILED(hr)) return false;
        use_d3d = true;
        return true;
    }

    // staging 纹理（CPU 可读）按输出尺寸惰性创建
    bool ensure_staging(int w, int h) {
        if (staging && staging_w == w && staging_h == h) return true;
        if (staging) { staging->Release(); staging = nullptr; }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = (UINT)w;
        desc.Height = (UINT)h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(d3d_dev->CreateTexture2D(&desc, nullptr, &staging))) return false;
        staging_w = w;
        staging_h = h;
        return true;
    }

    // 输出格式固定 NV12（打包格式还原在 GL shader 中做，CPU 不再转换）
    bool negotiate_output() {
        if (!mft) return false;
        for (DWORD i = 0;; ++i) {
            IMFMediaType* mt = nullptr;
            HRESULT hr = mft->GetOutputAvailableType(out_id, i, &mt);
            if (FAILED(hr) || !mt) break;
            GUID sub;
            mt->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == MFVideoFormat_NV12) {
                hr = mft->SetOutputType(out_id, mt, 0);
                if (SUCCEEDED(hr)) {
                    UINT32 w = 0, h = 0;
                    MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &w, &h);
                    out_subtype = sub;
                    out_w = (int)w;
                    out_h = (int)h;
                    mt->Release();
                    return true;
                }
            }
            mt->Release();
        }
        return false;
    }

    // 异步 MFT 事件泵：HaveOutput → 泵输出；StreamChange → 重协商输出类型。
    // wait=true 时阻塞等一个事件（超时 500ms 返回 false，防死等）。
    bool pump_events(VideoFrame& out, bool& got, bool wait) {
        if (!event_gen) return false;
        DWORD flags = wait ? 0 : MF_EVENT_FLAG_NO_WAIT;
        IMFMediaEvent* ev = nullptr;
        HRESULT hr = event_gen->GetEvent(flags, &ev);
        if (hr == MF_E_NO_EVENTS_AVAILABLE || hr == MF_E_SHUTDOWN) return false;
        if (FAILED(hr) || !ev) return false;
        MediaEventType type;
        ev->GetType(&type);
        ev->Release();
        if (type == METransformHaveOutput) {
            pump_outputs(out, got);
        }
        return true;
    }

    // ProcessOutput 泵：取尽所有已解码输出；解出帧时 got=true
    HRESULT pump_outputs(VideoFrame& out, bool& got) {
        for (;;) {
            // 每轮重查：stream change 后帧尺寸/所需缓冲可能已变
            MFT_OUTPUT_STREAM_INFO si{};
            mft->GetOutputStreamInfo(out_id, &si);
            DWORD buf_size = si.cbSize;
            if (buf_size == 0 && out_w > 0 && out_h > 0)
                buf_size = (DWORD)out_w * out_h * 3 / 2;  // NV12

            IMFSample* sample = nullptr;
            IMFMediaBuffer* mbuf = nullptr;
            if (buf_size > 0 && !(si.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
                if (FAILED(MFCreateMemoryBuffer(buf_size, &mbuf))) break;
                if (FAILED(MFCreateSample(&sample))) { mbuf->Release(); break; }
                sample->AddBuffer(mbuf);
                mbuf->Release();
            }
            MFT_OUTPUT_DATA_BUFFER ob{};
            ob.dwStreamID = out_id;
            ob.pSample = sample;
            DWORD status = 0;
            HRESULT hr = mft->ProcessOutput(0, 1, &ob, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                if (sample) sample->Release();
                return hr;
            }
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (sample) sample->Release();
                negotiate_output();
                continue;
            }
            if (FAILED(hr)) {
                if (sample) sample->Release();
                return hr;
            }
            // S_OK：转换输出
            if (ob.pSample) {
                if (use_d3d) {
                    // D3D11 硬解输出：NV12 D3D 纹理 → staging 回读 → 平面原样拷贝
                    IMFMediaBuffer* mbuf0 = nullptr;
                    IMFDXGIBuffer* dxbuf = nullptr;
                    ID3D11Texture2D* tex = nullptr;
                    UINT subidx = 0;
                    if (SUCCEEDED(ob.pSample->GetBufferByIndex(0, &mbuf0)) && mbuf0 &&
                        SUCCEEDED(mbuf0->QueryInterface(IID_PPV_ARGS(&dxbuf))) && dxbuf &&
                        SUCCEEDED(dxbuf->GetResource(IID_PPV_ARGS(&tex))) && tex &&
                        SUCCEEDED(dxbuf->GetSubresourceIndex(&subidx)) &&
                        out_subtype == MFVideoFormat_NV12 && out_w > 0 && out_h > 0 &&
                        ensure_staging(out_w, out_h)) {
                        d3d_ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, subidx,
                                                       nullptr);
                        D3D11_MAPPED_SUBRESOURCE map{};
                        if (SUCCEEDED(d3d_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
                            copy_nv12(static_cast<const std::uint8_t*>(map.pData),
                                      (LONG)map.RowPitch, out_w, out_h, out);
                            d3d_ctx->Unmap(staging, 0);
                            got = true;
                        }
                    }
                    if (tex) tex->Release();
                    if (dxbuf) dxbuf->Release();
                    if (mbuf0) mbuf0->Release();
                } else {
                IMFMediaBuffer* buf = nullptr;
                if (SUCCEEDED(ob.pSample->ConvertToContiguousBuffer(&buf)) && buf) {
                    IMF2DBuffer* buf2d = nullptr;
                    std::uint8_t* base = nullptr;
                    LONG stride = 0;
                    bool locked = false;
                    if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&buf2d))) && buf2d) {
                        locked = SUCCEEDED(buf2d->Lock2D(&base, &stride));
                    }
                    if (!locked) {
                        // 退回连续缓冲 + 自算默认 stride（MinGW 无 MFGetDefaultStride）
                        DWORD maxlen = 0;
                        if (SUCCEEDED(buf->Lock(&base, &maxlen, nullptr))) {
                            stride = out_w;  // NV12 Y 平面默认 stride
                            locked = true;
                        }
                    }
                    if (locked && base && out_w > 0 && out_h > 0 &&
                        out_subtype == MFVideoFormat_NV12) {
                        copy_nv12(base, stride, out_w, out_h, out);
                        got = true;
                    }
                    if (locked) {
                        if (buf2d) buf2d->Unlock2D();
                        else buf->Unlock();
                    }
                    if (buf2d) buf2d->Release();
                    buf->Release();
                }
                }
                ob.pSample->Release();
            }
            if (ob.pEvents) ob.pEvents->Release();
            // 继续泵直到 NEED_MORE_INPUT
        }
        return S_OK;
    }
};

H264Decoder::H264Decoder() : m(std::make_unique<Impl>()) {}
H264Decoder::~H264Decoder() { shutdown(); }

bool H264Decoder::init() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    m->com_inited = SUCCEEDED(hr) || hr == S_FALSE;
    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[decoder] MFStartup failed: 0x%08lx\n", (unsigned long)hr);
        return false;
    }
    m->mf_started = true;

    // 优先硬件解码（Intel QSV 等）：枚举硬件解码 MFT，优先名字带 Intel/QSV 的。
    // 硬件 MFT 多为异步（ASYNCMFT），需事件驱动，见 decode()。
    {
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        MFT_REGISTER_TYPE_INFO in = { MFMediaType_Video, MFVideoFormat_H264 };
        MFT_REGISTER_TYPE_INFO out = { MFMediaType_Video, MFVideoFormat_NV12 };
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                       MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT,
                       &in, &out, &acts, &count);
        if (SUCCEEDED(hr) && count > 0) {
            // 优先 Intel QSV，其次第一个
            UINT32 pick = 0;
            for (UINT32 i = 0; i < count; ++i) {
                LPWSTR name = nullptr;
                UINT32 len = 0;
                if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute,
                                                          &name, &len)) && name) {
                    std::wstring wname(name, len);
                    CoTaskMemFree(name);
                    if (wname.find(L"Intel") != std::wstring::npos) { pick = i; break; }
                }
            }
            LPWSTR name = nullptr;
            UINT32 len = 0;
            if (SUCCEEDED(acts[pick]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute,
                                                         &name, &len)) && name) {
                std::fprintf(stderr, "[decoder] using HW MFT: %ls\n", name);
                CoTaskMemFree(name);
            }
            UINT32 mft_flags = 0;
            acts[pick]->GetUINT32(MF_TRANSFORM_FLAGS_Attribute, &mft_flags);
            m->async_mode = (mft_flags & MFT_ENUM_FLAG_ASYNCMFT) != 0;
            hr = acts[pick]->ActivateObject(IID_PPV_ARGS(&m->mft));
            for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
        }
        if (acts) CoTaskMemFree(acts);
    }
    // 回退：MS 软件解码 MFT（同步）
    if (!m->mft) {
        hr = CoCreateInstance(CLSID_MSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&m->mft));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[decoder] create H264 MFT failed: 0x%08lx\n",
                         (unsigned long)hr);
            return false;
        }
        m->async_mode = false;
        std::fprintf(stderr, "[decoder] using Microsoft software H264 decoder\n");
    }

    if (m->async_mode) {
        // 异步 MFT：取事件源，发流式开始消息
        m->mft->QueryInterface(IID_PPV_ARGS(&m->event_gen));
    }

    m->mft->GetStreamIDs(1, &m->in_id, 1, &m->out_id);
    enable_low_latency(m->mft);

    // 尝试给 MFT 挂 D3D11 设备管理器（GPU 解码）；失败则纯软解
    if (m->init_d3d()) {
        std::fprintf(stderr, "[decoder] D3D11 hardware acceleration enabled\n");
    } else {
        std::fprintf(stderr, "[decoder] D3D11 unavailable, software decode\n");
    }

    // 输入类型：H264（Annex-B 字节流）优先——MS H.264 decoder 的 AVC1 子类型
    // 期望 AVCC（长度前缀）码流，喂 Annex-B 会静默无输出；H264 子类型才是 Annex-B。
    // 帧尺寸给个上限值，实际由 SPS 决定。
    IMFMediaType* in_mt = nullptr;
    MFCreateMediaType(&in_mt);
    in_mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in_mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(in_mt, MF_MT_FRAME_SIZE, 1920, 1080);
    hr = m->mft->SetInputType(m->in_id, in_mt, 0);
    if (FAILED(hr)) {
        in_mt->SetGUID(MF_MT_SUBTYPE, kMFVideoFormat_AVC1);
        hr = m->mft->SetInputType(m->in_id, in_mt, 0);
    }
    in_mt->Release();
    if (FAILED(hr)) {
        std::fprintf(stderr, "[decoder] SetInputType failed: 0x%08lx\n", (unsigned long)hr);
        return false;
    }

    // 预先尝试设输出 NV12（通常要 stream change 后才能成功，失败无碍）
    m->negotiate_output();

    if (m->async_mode) {
        m->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        m->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
    return true;
}

bool H264Decoder::decode(const std::uint8_t* data, std::size_t size, VideoFrame& out) {
    if (!m->mft || !data || size == 0) return false;

    IMFMediaBuffer* mbuf = nullptr;
    HRESULT hr = MFCreateMemoryBuffer((DWORD)size, &mbuf);
    if (FAILED(hr)) return false;
    std::uint8_t* p = nullptr;
    mbuf->Lock(&p, nullptr, nullptr);
    std::memcpy(p, data, size);
    mbuf->Unlock();
    mbuf->SetCurrentLength((DWORD)size);

    IMFSample* sample = nullptr;
    MFCreateSample(&sample);
    sample->AddBuffer(mbuf);
    mbuf->Release();

    bool got = false;
    if (m->async_mode) {
        // 异步 MFT（QSV）：先清已就绪事件（可能有 HaveOutput），再喂输入；
        // NOTACCEPTING 时阻塞等事件（NeedInput/HaveOutput），500ms 超时弃帧。
        while (m->pump_events(out, got, false)) {}
        hr = m->mft->ProcessInput(m->in_id, sample, 0);
        int waits = 0;
        while (hr == MF_E_NOTACCEPTING && waits < 5) {
            if (!m->pump_events(out, got, true)) ++waits;
            hr = m->mft->ProcessInput(m->in_id, sample, 0);
        }
        sample->Release();
        if (hr == MF_E_NOTACCEPTING) {
            std::fprintf(stderr, "[decoder] async MFT input starved, frame dropped\n");
            return false;
        }
        if (FAILED(hr)) {
            std::fprintf(stderr, "[decoder] ProcessInput failed: 0x%08lx\n",
                         (unsigned long)hr);
            return false;
        }
        while (m->pump_events(out, got, false)) {}
        return got;
    }

    hr = m->mft->ProcessInput(m->in_id, sample, 0);
    // 解码器输出未取尽时拒绝输入：先泵出再重试
    while (hr == MF_E_NOTACCEPTING) {
        m->pump_outputs(out, got);
        hr = m->mft->ProcessInput(m->in_id, sample, 0);
    }
    sample->Release();
    if (FAILED(hr)) {
        std::fprintf(stderr, "[decoder] ProcessInput failed: 0x%08lx\n", (unsigned long)hr);
        return false;
    }
    m->pump_outputs(out, got);
    return got;
}

void H264Decoder::shutdown() {
    if (m->event_gen) { m->event_gen->Release(); m->event_gen = nullptr; }
    if (m->mft) { m->mft->Release(); m->mft = nullptr; }
    if (m->staging) { m->staging->Release(); m->staging = nullptr; }
    if (m->dxgi_mgr) { m->dxgi_mgr->Release(); m->dxgi_mgr = nullptr; }
    if (m->d3d_ctx) { m->d3d_ctx->Release(); m->d3d_ctx = nullptr; }
    if (m->d3d_dev) { m->d3d_dev->Release(); m->d3d_dev = nullptr; }
    m->use_d3d = false;
    if (m->mf_started) { MFShutdown(); m->mf_started = false; }
    if (m->com_inited) { CoUninitialize(); m->com_inited = false; }
}
