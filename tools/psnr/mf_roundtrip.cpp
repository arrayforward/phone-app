// PSNR 对比工具·H.264 编解码往返（Media Foundation，host 端 MinGW g++）：
//   mf_roundtrip in.yuv W H bitrate_bps out.yuv
// I420 输入 → MF H.264 硬/软编码（CBR）→ MF 解码 → I420 输出（NV12 拆 UV）。
// 用于对比"双 YUV420 打包"与"单 YUV420"在相同编码器相同码率下的失真。
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <codecapi.h>
#include <strmif.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

// MinGW 的 uuid/strmiids 缺 IID_ICodecAPI：本地定义（值与 Windows SDK 一致）
static const GUID kIID_ICodecAPI =
    { 0x901DB4C7, 0x31CE, 0x41A2, { 0x85, 0xDC, 0x8F, 0xA0, 0xBF, 0x41, 0xB8, 0xDA } };

namespace {

std::vector<std::uint8_t> read_all(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open %s failed\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<std::uint8_t> buf((size_t)n);
    if (fread(buf.data(), 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read failed\n"); exit(1); }
    fclose(f);
    return buf;
}

void write_all(const char* path, const std::vector<std::uint8_t>& b) {
    FILE* f = fopen(path, "wb");
    if (!f || fwrite(b.data(), 1, b.size(), f) != b.size()) { fprintf(stderr, "write failed\n"); exit(1); }
    fclose(f);
}

IMFSample* make_sample(const std::uint8_t* data, DWORD size) {
    IMFMediaBuffer* mbuf = nullptr;
    MFCreateMemoryBuffer(size, &mbuf);
    std::uint8_t* p = nullptr;
    mbuf->Lock(&p, nullptr, nullptr);
    std::memcpy(p, data, size);
    mbuf->Unlock();
    mbuf->SetCurrentLength(size);
    IMFSample* s = nullptr;
    MFCreateSample(&s);
    s->AddBuffer(mbuf);
    mbuf->Release();
    return s;
}

// 编码器：I420 单帧 → H.264（输出 AVCC 字节流 + SPS/PPS 序列头）
bool encode_one(const std::vector<std::uint8_t>& yuv, int w, int h, DWORD bitrate,
                std::vector<std::uint8_t>& stream, std::vector<std::uint8_t>& seqhdr) {
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    MFT_REGISTER_TYPE_INFO out_info = { MFMediaType_Video, MFVideoFormat_H264 };
    MFT_REGISTER_TYPE_INFO in_info = { MFMediaType_Video, MFVideoFormat_I420 };
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                           MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT,
                           &in_info, &out_info, &acts, &count);
    if (FAILED(hr) || count == 0) {
        // 输入类型换 NV12 再找
        in_info = { MFMediaType_Video, MFVideoFormat_NV12 };
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                       MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT,
                       &in_info, &out_info, &acts, &count);
    }
    if (FAILED(hr) || count == 0) {
        fprintf(stderr, "no H264 encoder MFT (hr=0x%08lx)\n", (unsigned long)hr);
        return false;
    }
    // 选纯软件同步 MFT（QSV 硬编 MFT 对输入类型/参数有额外要求，本工具求稳）
    IMFTransform* enc = nullptr;
    for (UINT32 i = 0; i < count && !enc; ++i) {
        UINT32 fl = 0;
        acts[i]->GetUINT32(MF_TRANSFORM_FLAGS_Attribute, &fl);
        LPWSTR name = nullptr; UINT32 len = 0;
        acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len);
        bool usable = (fl & (MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE)) == 0;
        fprintf(stderr, "encoder[%u] flags=0x%x %ls%s\n", i, fl, name ? name : L"?",
                usable ? " (selected)" : " (skip)");
        if (name) CoTaskMemFree(name);
        if (usable) acts[i]->ActivateObject(IID_PPV_ARGS(&enc));
    }
    for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    if (!enc) { fprintf(stderr, "no sync SW encoder MFT\n"); return false; }

    // CBR 码率
    ICodecAPI* api = nullptr;
    if (SUCCEEDED(enc->QueryInterface(kIID_ICodecAPI, (void**)&api)) && api) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = 0;  // CBR
        api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
        v.ulVal = bitrate;
        api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);
        VariantClear(&v);
        api->Release();
    }

    // 编码器 MFT：先设输出（手工构造裸类型即可）再设输入
    IMFMediaType* mt = nullptr;
    MFCreateMediaType(&mt);
    mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    mt->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    mt->SetUINT32(MF_MT_MPEG2_PROFILE, 100);  // eAVEncH264VProfile_High
    MFSetAttributeSize(mt, MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(mt, MF_MT_FRAME_RATE, 30, 1);
    MFSetAttributeRatio(mt, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = enc->SetOutputType(0, mt, 0);
    mt->Release();
    if (FAILED(hr)) { fprintf(stderr, "enc SetOutputType 0x%08lx\n", (unsigned long)hr); enc->Release(); return false; }

    bool input_nv12 = false;
    MFCreateMediaType(&mt);
    mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_I420);
    MFSetAttributeSize(mt, MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(mt, MF_MT_FRAME_RATE, 30, 1);
    MFSetAttributeRatio(mt, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = enc->SetInputType(0, mt, 0);
    if (FAILED(hr)) {
        mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        hr = enc->SetInputType(0, mt, 0);
        input_nv12 = SUCCEEDED(hr);
    }
    mt->Release();
    if (FAILED(hr)) { fprintf(stderr, "enc SetInputType 0x%08lx\n", (unsigned long)hr); enc->Release(); return false; }

    // 取序列头（SPS/PPS，AVCC 模式）
    IMFMediaType* out_mt = nullptr;
    if (SUCCEEDED(enc->GetOutputCurrentType(0, &out_mt)) && out_mt) {
        UINT32 seq_len = 0;
        if (SUCCEEDED(out_mt->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &seq_len)) && seq_len > 0) {
            seqhdr.resize(seq_len);
            out_mt->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, seqhdr.data(), seq_len, nullptr);
        }
        out_mt->Release();
    }

    enc->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    enc->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    // NV12 输入时把 I420 的 U/V 平面交错成 UV
    std::vector<std::uint8_t> nv12;
    const std::uint8_t* feed = yuv.data();
    DWORD feed_size = (DWORD)yuv.size();
    if (input_nv12) {
        size_t n = (size_t)w * h;
        nv12 = yuv;  // 先拷 Y
        nv12.resize(n * 3 / 2);
        const std::uint8_t* up = yuv.data() + n;
        const std::uint8_t* vp = up + n / 4;
        std::uint8_t* uv = nv12.data() + n;
        for (int i = 0; i < w * h / 4; ++i) {
            uv[i * 2] = up[i];
            uv[i * 2 + 1] = vp[i];
        }
        feed = nv12.data();
        feed_size = (DWORD)nv12.size();
    }
    IMFSample* in = make_sample(feed, feed_size);
    hr = enc->ProcessInput(0, in, 0);
    in->Release();
    if (FAILED(hr)) { fprintf(stderr, "enc ProcessInput 0x%08lx\n", (unsigned long)hr); enc->Release(); return false; }
    enc->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);

    // 泵输出
    MFT_OUTPUT_STREAM_INFO si{};
    enc->GetOutputStreamInfo(0, &si);
    for (;;) {
        IMFSample* os = nullptr;
        if (!(si.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            IMFMediaBuffer* ob = nullptr;
            MFCreateMemoryBuffer(si.cbSize ? si.cbSize : (DWORD)(w * h), &ob);
            MFCreateSample(&os);
            os->AddBuffer(ob);
            ob->Release();
        }
        MFT_OUTPUT_DATA_BUFFER odb{};
        odb.pSample = os;
        DWORD status = 0;
        hr = enc->ProcessOutput(0, 1, &odb, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) { if (os) os->Release(); break; }
        if (FAILED(hr)) { if (os) os->Release(); break; }
        if (odb.pSample) {
            IMFMediaBuffer* cb = nullptr;
            if (SUCCEEDED(odb.pSample->ConvertToContiguousBuffer(&cb)) && cb) {
                std::uint8_t* p = nullptr;
                DWORD len = 0;
                cb->Lock(&p, nullptr, &len);
                stream.insert(stream.end(), p, p + len);
                cb->Unlock();
                cb->Release();
            }
            odb.pSample->Release();
        }
        if (odb.pEvents) odb.pEvents->Release();
    }
    enc->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    enc->Release();
    return !stream.empty();
}

// 解码器：H.264 AVCC（带序列头）→ I420 平面（单帧）
bool decode_one(const std::vector<std::uint8_t>& stream, const std::vector<std::uint8_t>& seqhdr,
                int w, int h, std::vector<std::uint8_t>& yuv_out) {
    IMFTransform* dec = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_MSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dec));
    if (FAILED(hr)) { fprintf(stderr, "no H264 decoder 0x%08lx\n", (unsigned long)hr); return false; }

    DWORD in_id = 0, out_id = 0;
    dec->GetStreamIDs(1, &in_id, 1, &out_id);

    IMFMediaType* mt = nullptr;
    MFCreateMediaType(&mt);
    mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (!seqhdr.empty())
        mt->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, seqhdr.data(), (UINT32)seqhdr.size());
    MFSetAttributeSize(mt, MF_MT_FRAME_SIZE, w, h);
    hr = dec->SetInputType(in_id, mt, 0);
    mt->Release();
    if (FAILED(hr)) { fprintf(stderr, "dec SetInputType 0x%08lx\n", (unsigned long)hr); dec->Release(); return false; }

    // 输出 NV12
    bool ok = false;
    for (DWORD i = 0;; ++i) {
        IMFMediaType* omt = nullptr;
        if (FAILED(dec->GetOutputAvailableType(out_id, i, &omt)) || !omt) break;
        GUID sub;
        omt->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFVideoFormat_NV12) {
            hr = dec->SetOutputType(out_id, omt, 0);
            omt->Release();
            ok = SUCCEEDED(hr);
            break;
        }
        omt->Release();
    }
    if (!ok) { fprintf(stderr, "dec no NV12 output\n"); dec->Release(); return false; }

    IMFSample* in = make_sample(stream.data(), (DWORD)stream.size());
    hr = dec->ProcessInput(in_id, in, 0);
    in->Release();
    if (FAILED(hr)) { fprintf(stderr, "dec ProcessInput 0x%08lx\n", (unsigned long)hr); dec->Release(); return false; }

    yuv_out.assign((size_t)w * h * 3 / 2, 0);
    bool got = false;
    for (int spins = 0; spins < 100 && !got; ++spins) {
        MFT_OUTPUT_STREAM_INFO si{};
        dec->GetOutputStreamInfo(out_id, &si);
        IMFSample* os = nullptr;
        IMFMediaBuffer* ob = nullptr;
        MFCreateMemoryBuffer(si.cbSize ? si.cbSize : (DWORD)(w * h * 2), &ob);
        MFCreateSample(&os);
        os->AddBuffer(ob);
        ob->Release();
        MFT_OUTPUT_DATA_BUFFER odb{};
        odb.pSample = os;
        DWORD status = 0;
        hr = dec->ProcessOutput(0, 1, &odb, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) { os->Release(); break; }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            os->Release();
            // 重设输出类型后继续
            for (DWORD i = 0;; ++i) {
                IMFMediaType* omt = nullptr;
                if (FAILED(dec->GetOutputAvailableType(out_id, i, &omt)) || !omt) break;
                GUID sub;
                omt->GetGUID(MF_MT_SUBTYPE, &sub);
                if (sub == MFVideoFormat_NV12) { dec->SetOutputType(out_id, omt, 0); omt->Release(); break; }
                omt->Release();
            }
            continue;
        }
        if (FAILED(hr)) { os->Release(); break; }
        if (odb.pSample) {
            IMFMediaBuffer* cb = nullptr;
            if (SUCCEEDED(odb.pSample->ConvertToContiguousBuffer(&cb)) && cb) {
                IMF2DBuffer* b2d = nullptr;
                std::uint8_t* base = nullptr;
                LONG stride = 0;
                bool locked = false;
                if (SUCCEEDED(cb->QueryInterface(IID_PPV_ARGS(&b2d))) && b2d)
                    locked = SUCCEEDED(b2d->Lock2D(&base, &stride));
                if (!locked) {
                    DWORD maxlen = 0;
                    if (SUCCEEDED(cb->Lock(&base, &maxlen, nullptr))) { stride = w; locked = true; }
                }
                if (locked && base) {
                    // NV12 → I420：Y 直拷，UV 拆交错
                    std::uint8_t* yp = yuv_out.data();
                    std::uint8_t* up = yp + (size_t)w * h;
                    std::uint8_t* vp = up + (size_t)w * h / 4;
                    for (int y = 0; y < h; ++y)
                        std::memcpy(yp + (size_t)y * w, base + (ptrdiff_t)y * stride, w);
                    const std::uint8_t* uvsrc = base + (ptrdiff_t)stride * h;
                    for (int y = 0; y < h / 2; ++y)
                        for (int x = 0; x < w / 2; ++x) {
                            up[(size_t)y * (w / 2) + x] = uvsrc[(ptrdiff_t)y * stride + x * 2];
                            vp[(size_t)y * (w / 2) + x] = uvsrc[(ptrdiff_t)y * stride + x * 2 + 1];
                        }
                    got = true;
                }
                if (locked) { if (b2d) b2d->Unlock2D(); else cb->Unlock(); }
                if (b2d) b2d->Release();
                cb->Release();
            }
            odb.pSample->Release();
        }
        if (odb.pEvents) odb.pEvents->Release();
    }
    dec->Release();
    return got;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: mf_roundtrip in.yuv W H bitrate_bps out.yuv\n");
        return 1;
    }
    int w = atoi(argv[2]);
    int h = atoi(argv[3]);
    DWORD bitrate = (DWORD)strtoul(argv[4], nullptr, 10);

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    auto yuv = read_all(argv[1]);
    std::vector<std::uint8_t> stream, seqhdr, decoded;
    if (!encode_one(yuv, w, h, bitrate, stream, seqhdr)) {
        fprintf(stderr, "encode failed\n");
        return 1;
    }
    printf("encoded %zuB (seqhdr %zuB)\n", stream.size(), seqhdr.size());
    if (!decode_one(stream, seqhdr, w, h, decoded)) {
        fprintf(stderr, "decode failed\n");
        return 1;
    }
    write_all(argv[5], decoded);
    printf("decoded -> %s (%zuB)\n", argv[5], decoded.size());

    MFShutdown();
    CoUninitialize();
    return 0;
}
