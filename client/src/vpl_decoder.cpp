// Intel oneVPL (libvpl) H.264 硬解器实现。
// 运行时 dlopen libvpl.dll；硬件优先（mfxImplDescription.Impl=HARDWARE），
// 会话建立失败即 init 返回 false，由 main 回退 Media Foundation。

#include "vpl_decoder.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "vpl/mfx.h"

namespace {

mfxStatus g_last_sts = MFX_ERR_NONE;

const char* sts_str(mfxStatus s) {
    switch (s) {
    case MFX_ERR_NONE: return "NONE";
    case MFX_ERR_MORE_DATA: return "MORE_DATA";
    case MFX_ERR_MORE_SURFACE: return "MORE_SURFACE";
    case MFX_ERR_DEVICE_LOST: return "DEVICE_LOST";
    case MFX_WRN_DEVICE_BUSY: return "WRN_DEVICE_BUSY";
    case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM: return "INCOMPATIBLE_VIDEO_PARAM";
    case MFX_ERR_INVALID_VIDEO_PARAM: return "INVALID_VIDEO_PARAM";
    case MFX_ERR_UNDEFINED_BEHAVIOR: return "UNDEFINED_BEHAVIOR";
    case MFX_ERR_NOT_INITIALIZED: return "NOT_INITIALIZED";
    default: return "?";
    }
}

} // namespace

struct VplDecoder::Impl {
    HMODULE dll = nullptr;

    // dispatcher / session
    decltype(&MFXLoad) Load = nullptr;
    decltype(&MFXUnload) Unload = nullptr;
    decltype(&MFXCreateConfig) CreateConfig = nullptr;
    decltype(&MFXSetConfigFilterProperty) SetConfigFilterProperty = nullptr;
    decltype(&MFXCreateSession) CreateSession = nullptr;
    decltype(&MFXClose) Close = nullptr;
    // decode
    decltype(&MFXVideoDECODE_DecodeHeader) DecodeHeader = nullptr;
    decltype(&MFXVideoDECODE_Init) DecInit = nullptr;
    decltype(&MFXVideoDECODE_DecodeFrameAsync) DecodeFrameAsync = nullptr;
    decltype(&MFXVideoDECODE_Close) DecClose = nullptr;
    decltype(&MFXVideoCORE_SyncOperation) SyncOperation = nullptr;

    mfxLoader loader = nullptr;
    mfxSession session = nullptr;
    mfxVideoParam params{};
    bool initialized = false;

    // 持久 bitstream 缓冲：DecodeFrameAsync 可能只消费部分数据，
    // 剩余字节留到下次 decode() 与新 AU 拼接后继续送
    std::vector<mfxU8> bs_buf;
    mfxBitstream bs{};

    void bs_append(const std::uint8_t* data, std::size_t size) {
        if (bs.DataLength == 0) {
            bs.DataOffset = 0;
            bs_buf.clear();
        } else if (bs.DataOffset > 0) {
            std::memmove(bs_buf.data(), bs_buf.data() + bs.DataOffset, bs.DataLength);
            bs.DataOffset = 0;
            bs_buf.resize(bs.DataLength);
        }
        bs_buf.insert(bs_buf.end(), data, data + size);
        bs.Data = bs_buf.data();
        bs.DataLength += (mfxU32)size;
        bs.MaxLength = (mfxU32)bs_buf.size();
        bs.DataFlag = MFX_BITSTREAM_COMPLETE_FRAME;
    }

    // 首帧（带 SPS/PPS 的 IDR）DecodeHeader 拿参数后 Init
    bool init_decoder() {
        mfxStatus sts = DecodeHeader(session, &bs, &params);
        if (sts == MFX_ERR_MORE_DATA) return false;  // SPS/PPS 还没到齐
        if (sts < 0) {
            std::fprintf(stderr, "[vpl] DecodeHeader failed: %s (%d)\n", sts_str(sts), (int)sts);
            return false;
        }
        sts = DecInit(session, &params);
        if (sts < 0) {
            std::fprintf(stderr, "[vpl] MFXVideoDECODE_Init failed: %s (%d)\n", sts_str(sts), (int)sts);
            return false;
        }
        initialized = true;
        std::fprintf(stderr, "[vpl] decoder initialized: %ux%u fourcc=%.4s\n",
                     params.mfx.FrameInfo.CropW, params.mfx.FrameInfo.CropH,
                     (const char*)&params.mfx.FrameInfo.FourCC);
        return true;
    }

    // 按 Pitch 从系统内存 surface 拷出 NV12（I420 则拼成交错 UV）
    bool copy_surface(mfxFrameSurface1* surf, VideoFrame& out) {
        const mfxFrameInfo& info = surf->Info;
        int w = info.CropW ? info.CropW : info.Width;
        int h = info.CropH ? info.CropH : info.Height;
        if (w <= 0 || h <= 0) return false;
        mfxU32 pitch = (mfxU32)surf->Data.PitchHigh << 16 | surf->Data.PitchLow;
        if (pitch < (mfxU32)w || !surf->Data.Y) return false;

        out.width = w;
        out.height = h;
        out.y.resize((std::size_t)w * h);
        for (int r = 0; r < h; ++r)
            std::memcpy(out.y.data() + (std::size_t)r * w, surf->Data.Y + (std::size_t)r * pitch, w);

        out.uv.resize((std::size_t)w * (h / 2));
        if (info.FourCC == MFX_FOURCC_NV12) {
            if (!surf->Data.UV) return false;
            for (int r = 0; r < h / 2; ++r)
                std::memcpy(out.uv.data() + (std::size_t)r * w, surf->Data.UV + (std::size_t)r * pitch, w);
        } else if (info.FourCC == MFX_FOURCC_I420) {  // IYUV：U/V 分平面，拼成交错
            if (!surf->Data.U || !surf->Data.V) return false;
            mfxU32 cpitch = pitch / 2;
            for (int r = 0; r < h / 2; ++r) {
                const mfxU8* u = surf->Data.U + (std::size_t)r * cpitch;
                const mfxU8* v = surf->Data.V + (std::size_t)r * cpitch;
                mfxU8* dst = out.uv.data() + (std::size_t)r * w;
                for (int c = 0; c < w / 2; ++c) {
                    dst[2 * c] = u[c];
                    dst[2 * c + 1] = v[c];
                }
            }
        } else {
            std::fprintf(stderr, "[vpl] unsupported fourcc %.4s\n", (const char*)&info.FourCC);
            return false;
        }
        return true;
    }
};

VplDecoder::VplDecoder() = default;

VplDecoder::~VplDecoder() { shutdown(); }

bool VplDecoder::init() {
    auto impl = std::make_unique<Impl>();

    impl->dll = LoadLibraryA("libvpl.dll");
    if (!impl->dll) {
        std::fprintf(stderr, "[vpl] LoadLibrary libvpl.dll failed (err=%lu)\n", GetLastError());
        return false;
    }
    HMODULE dll = impl->dll;
    bool ok = true;
#define VPL_LOAD(field, sym)                                              \
    do {                                                                  \
        impl->field = reinterpret_cast<decltype(impl->field)>(            \
            (void*)GetProcAddress(dll, sym));                             \
        if (!impl->field) {                                               \
            std::fprintf(stderr, "[vpl] GetProcAddress %s failed\n", sym); \
            ok = false;                                                   \
        }                                                                 \
    } while (0)
    VPL_LOAD(Load, "MFXLoad");
    VPL_LOAD(Unload, "MFXUnload");
    VPL_LOAD(CreateConfig, "MFXCreateConfig");
    VPL_LOAD(SetConfigFilterProperty, "MFXSetConfigFilterProperty");
    VPL_LOAD(CreateSession, "MFXCreateSession");
    VPL_LOAD(Close, "MFXClose");
    VPL_LOAD(DecodeHeader, "MFXVideoDECODE_DecodeHeader");
    VPL_LOAD(DecInit, "MFXVideoDECODE_Init");
    VPL_LOAD(DecodeFrameAsync, "MFXVideoDECODE_DecodeFrameAsync");
    VPL_LOAD(DecClose, "MFXVideoDECODE_Close");
    VPL_LOAD(SyncOperation, "MFXVideoCORE_SyncOperation");
#undef VPL_LOAD
    if (!ok) {
        FreeLibrary(dll);
        return false;
    }

    impl->loader = impl->Load();
    if (!impl->loader) {
        std::fprintf(stderr, "[vpl] MFXLoad failed\n");
        FreeLibrary(dll);
        return false;
    }

    // 硬件实现优先：filter mfxImplDescription.Impl = HARDWARE
    mfxConfig cfg = impl->CreateConfig(impl->loader);
    if (cfg) {
        mfxVariant v{};
        v.Type = MFX_VARIANT_TYPE_U32;
        v.Data.U32 = MFX_IMPL_TYPE_HARDWARE;
        impl->SetConfigFilterProperty(cfg, (const mfxU8*)"mfxImplDescription.Impl", v);
    }
    mfxStatus sts = impl->CreateSession(impl->loader, 0, &impl->session);
    if (sts != MFX_ERR_NONE || !impl->session) {
        // 无硬件实现：不再尝试软件路径，直接让上层回退 MF
        std::fprintf(stderr, "[vpl] hardware session unavailable (%s %d), fall back to MF\n",
                     sts_str(sts), (int)sts);
        impl->Unload(impl->loader);
        FreeLibrary(dll);
        return false;
    }

    // 低时延：AsyncDepth=1，系统内存输出（拷出 NV12 平面）
    std::memset(&impl->params, 0, sizeof(impl->params));
    impl->params.mfx.CodecId = MFX_CODEC_AVC;
    impl->params.AsyncDepth = 1;
    impl->params.IOPattern = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;

    m = std::move(impl);
    std::fprintf(stderr, "[vpl] session created (hardware)\n");
    return true;
}

void VplDecoder::shutdown() {
    if (!m) return;
    if (m->initialized && m->session) m->DecClose(m->session);
    m->initialized = false;
    if (m->session) {
        m->Close(m->session);
        m->session = nullptr;
    }
    if (m->loader) {
        m->Unload(m->loader);
        m->loader = nullptr;
    }
    if (m->dll) {
        FreeLibrary(m->dll);
        m->dll = nullptr;
    }
    m.reset();
}

bool VplDecoder::decode(const std::uint8_t* data, std::size_t size, VideoFrame& out) {
    if (!m || !m->session) return false;

    m->bs_append(data, size);

    if (!m->initialized && !m->init_decoder()) return false;

    for (int retry = 0; retry < 16; ++retry) {
        mfxFrameSurface1* surf = nullptr;
        mfxSyncPoint syncp = nullptr;
        mfxStatus sts = m->DecodeFrameAsync(m->session,
            m->bs.DataLength ? &m->bs : nullptr, nullptr, &surf, &syncp);

        if (sts == MFX_WRN_DEVICE_BUSY) {  // 硬件忙：稍等重试
            Sleep(1);
            continue;
        }
        if (sts == MFX_ERR_MORE_DATA || sts == MFX_ERR_MORE_SURFACE) {
            static int more_data_cnt = 0;
            if (++more_data_cnt % 30 == 1)
                std::fprintf(stderr, "[vpl] DecodeFrameAsync %s x%d (bs_len=%u)\n",
                             sts_str(sts), more_data_cnt, m->bs.DataLength);
            return false;  // 本次输入被内部缓冲，帧会在后续 decode 调用吐出
        }
        if (sts < 0) {
            if (sts != g_last_sts) {  // 同类错误只打一次，避免刷屏
                std::fprintf(stderr, "[vpl] DecodeFrameAsync failed: %s (%d)\n",
                             sts_str(sts), (int)sts);
                g_last_sts = sts;
            }
            return false;
        }
        if (!syncp) return false;

        sts = m->SyncOperation(m->session, syncp, 1000);
        if (sts != MFX_ERR_NONE) {
            std::fprintf(stderr, "[vpl] SyncOperation failed: %s (%d)\n", sts_str(sts), (int)sts);
            return false;
        }
        if (!surf) return false;

        // oneVPL 2.x 运行时输出的是内部显存 surface：拷出前必须
        // FrameInterface->Map(READ) 触发 GPU→CPU 拷贝（否则 Pitch/Data 全为 0）
        bool copied = false;
        if (surf->FrameInterface) {
            sts = surf->FrameInterface->Map(surf, MFX_MAP_READ);
            if (sts == MFX_ERR_NONE) {
                copied = m->copy_surface(surf, out);
                surf->FrameInterface->Unmap(surf);
            } else {
                std::fprintf(stderr, "[vpl] surface Map failed: %s (%d)\n",
                             sts_str(sts), (int)sts);
            }
            surf->FrameInterface->Release(surf);
        } else {
            // 旧 MSDK 运行时：系统内存 surface，直接可读
            copied = m->copy_surface(surf, out);
        }
        return copied;
    }
    return false;  // 连续 DEVICE_BUSY，放弃本次
}
