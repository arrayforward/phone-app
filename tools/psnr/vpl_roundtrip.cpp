// PSNR 对比工具·H.264 编解码往返（Intel oneVPL 硬编硬解，运行时 dlopen libvpl.dll）：
//   vpl_roundtrip in.yuv W H bitrate_bps out.yuv
// I420 输入 → oneVPL H.264 CBR 编码 → oneVPL 解码 → I420 输出。
// 用于对比"双 YUV420 打包"与"单 YUV420"在相同编解码器相同码率下的失真。
#include <windows.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#include "vpl/mfx.h"

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

struct Vpl {
    HMODULE dll = nullptr;
    decltype(&MFXLoad) Load = nullptr;
    decltype(&MFXUnload) Unload = nullptr;
    decltype(&MFXCreateConfig) CreateConfig = nullptr;
    decltype(&MFXSetConfigFilterProperty) SetConfigFilterProperty = nullptr;
    decltype(&MFXCreateSession) CreateSession = nullptr;
    decltype(&MFXClose) Close = nullptr;
    decltype(&MFXVideoENCODE_Init) EncInit = nullptr;
    decltype(&MFXVideoENCODE_EncodeFrameAsync) EncodeFrameAsync = nullptr;
    decltype(&MFXVideoENCODE_Close) EncClose = nullptr;
    decltype(&MFXVideoENCODE_Query) EncQuery = nullptr;
    decltype(&MFXVideoDECODE_DecodeHeader) DecodeHeader = nullptr;
    decltype(&MFXVideoDECODE_Init) DecInit = nullptr;
    decltype(&MFXVideoDECODE_DecodeFrameAsync) DecodeFrameAsync = nullptr;
    decltype(&MFXVideoDECODE_Close) DecClose = nullptr;
    decltype(&MFXVideoCORE_SyncOperation) SyncOperation = nullptr;

    mfxLoader loader = nullptr;
    mfxSession session = nullptr;

    bool open() {
        dll = LoadLibraryA("libvpl.dll");
        if (!dll) { fprintf(stderr, "libvpl.dll not found\n"); return false; }
        Load = (decltype(&MFXLoad))GetProcAddress(dll, "MFXLoad");
        Unload = (decltype(&MFXUnload))GetProcAddress(dll, "MFXUnload");
        CreateConfig = (decltype(&MFXCreateConfig))GetProcAddress(dll, "MFXCreateConfig");
        SetConfigFilterProperty = (decltype(&MFXSetConfigFilterProperty))
            GetProcAddress(dll, "MFXSetConfigFilterProperty");
        CreateSession = (decltype(&MFXCreateSession))GetProcAddress(dll, "MFXCreateSession");
        Close = (decltype(&MFXClose))GetProcAddress(dll, "MFXClose");
        EncInit = (decltype(&MFXVideoENCODE_Init))GetProcAddress(dll, "MFXVideoENCODE_Init");
        EncodeFrameAsync = (decltype(&MFXVideoENCODE_EncodeFrameAsync))
            GetProcAddress(dll, "MFXVideoENCODE_EncodeFrameAsync");
        EncClose = (decltype(&MFXVideoENCODE_Close))GetProcAddress(dll, "MFXVideoENCODE_Close");
        EncQuery = (decltype(&MFXVideoENCODE_Query))GetProcAddress(dll, "MFXVideoENCODE_Query");
        DecodeHeader = (decltype(&MFXVideoDECODE_DecodeHeader))
            GetProcAddress(dll, "MFXVideoDECODE_DecodeHeader");
        DecInit = (decltype(&MFXVideoDECODE_Init))GetProcAddress(dll, "MFXVideoDECODE_Init");
        DecodeFrameAsync = (decltype(&MFXVideoDECODE_DecodeFrameAsync))
            GetProcAddress(dll, "MFXVideoDECODE_DecodeFrameAsync");
        DecClose = (decltype(&MFXVideoDECODE_Close))GetProcAddress(dll, "MFXVideoDECODE_Close");
        SyncOperation = (decltype(&MFXVideoCORE_SyncOperation))
            GetProcAddress(dll, "MFXVideoCORE_SyncOperation");
        return Load && CreateConfig && CreateSession && EncInit && DecInit && SyncOperation;
    }

    bool new_hw_session() {
        loader = Load();
        if (!loader) return false;
        mfxConfig cfg = CreateConfig(loader);
        mfxVariant v{};
        v.Type = MFX_VARIANT_TYPE_U32;
        v.Data.U32 = MFX_IMPL_TYPE_HARDWARE;
        SetConfigFilterProperty(cfg, (mfxU8*)"mfxImplDescription.Impl", v);
        mfxStatus sts = CreateSession(loader, 0, &session);
        return sts == MFX_ERR_NONE && session;
    }
};

// I420 → NV12 表面（交错 UV）
void fill_surface_nv12(mfxFrameSurface1& surf, const std::vector<std::uint8_t>& i420,
                       int w, int h, std::vector<std::uint8_t>& backing) {
    size_t n = (size_t)w * h;
    backing.resize(n * 3 / 2);
    std::memcpy(backing.data(), i420.data(), n);  // Y
    const std::uint8_t* up = i420.data() + n;
    const std::uint8_t* vp = up + n / 4;
    std::uint8_t* uv = backing.data() + n;
    for (size_t i = 0; i < n / 4; ++i) {
        uv[i * 2] = up[i];
        uv[i * 2 + 1] = vp[i];
    }
    std::memset(&surf, 0, sizeof(surf));
    surf.Info.FourCC = MFX_FOURCC_NV12;
    surf.Info.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    surf.Info.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    surf.Info.Width = (mfxU16)((w + 15) & ~15);
    surf.Info.Height = (mfxU16)((h + 15) & ~15);
    surf.Info.CropW = (mfxU16)w;
    surf.Info.CropH = (mfxU16)h;
    surf.Info.FrameRateExtN = 30;
    surf.Info.FrameRateExtD = 1;
    surf.Data.PitchLow = (mfxU16)(w & 0xFFFF);
    surf.Data.PitchHigh = (mfxU16)(w >> 16);
    surf.Data.Y = backing.data();
    surf.Data.UV = backing.data() + n;
}

// NV12 surface → I420
void surface_to_i420(mfxFrameSurface1* surf, int w, int h, std::vector<std::uint8_t>& out) {
    mfxU32 pitch = ((mfxU32)surf->Data.PitchHigh << 16) | surf->Data.PitchLow;
    out.assign((size_t)w * h * 3 / 2, 0);
    for (int r = 0; r < h; ++r)
        std::memcpy(out.data() + (size_t)r * w, surf->Data.Y + (size_t)r * pitch, w);
    std::uint8_t* up = out.data() + (size_t)w * h;
    std::uint8_t* vp = up + (size_t)w * h / 4;
    const std::uint8_t* uv = surf->Data.UV;
    for (int r = 0; r < h / 2; ++r)
        for (int x = 0; x < w / 2; ++x) {
            up[(size_t)r * (w / 2) + x] = uv[(size_t)r * pitch + x * 2];
            vp[(size_t)r * (w / 2) + x] = uv[(size_t)r * pitch + x * 2 + 1];
        }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: vpl_roundtrip in.yuv W H bitrate_bps out.yuv\n");
        return 1;
    }
    int w = atoi(argv[2]);
    int h = atoi(argv[3]);
    mfxU32 target_kbps = (mfxU32)(strtoul(argv[4], nullptr, 10) / 1000);

    auto yuv = read_all(argv[1]);

    Vpl v;
    if (!v.open() || !v.new_hw_session()) {
        fprintf(stderr, "vpl hw session failed\n");
        return 1;
    }

    // ---- 编码 ----
    mfxVideoParam enc_par{};
    enc_par.mfx.CodecId = MFX_CODEC_AVC;
    enc_par.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
    enc_par.mfx.TargetKbps = target_kbps;
    enc_par.mfx.TargetUsage = MFX_TARGETUSAGE_BALANCED;
    enc_par.mfx.CodecProfile = MFX_PROFILE_AVC_HIGH;
    enc_par.mfx.CodecLevel = MFX_LEVEL_AVC_41;
    enc_par.mfx.GopPicSize = 60;
    enc_par.mfx.GopRefDist = 1;
    enc_par.mfx.IdrInterval = 0;
    enc_par.mfx.NumRefFrame = 2;
    enc_par.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    enc_par.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    enc_par.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    enc_par.mfx.FrameInfo.Width = (mfxU16)((w + 15) & ~15);
    enc_par.mfx.FrameInfo.Height = (mfxU16)((h + 15) & ~15);
    enc_par.mfx.FrameInfo.CropW = (mfxU16)w;
    enc_par.mfx.FrameInfo.CropH = (mfxU16)h;
    enc_par.mfx.FrameInfo.FrameRateExtN = 30;
    enc_par.mfx.FrameInfo.FrameRateExtD = 1;
    enc_par.mfx.FrameInfo.AspectRatioW = 1;
    enc_par.mfx.FrameInfo.AspectRatioH = 1;
    enc_par.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY | MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    enc_par.AsyncDepth = 1;

    // 先 Query 让编码器修正参数（诊断 EncInit -15）
    mfxVideoParam q = enc_par;
    mfxStatus qsts = v.EncQuery(v.session, &enc_par, &q);
    fprintf(stderr, "EncQuery %d: FourCC=%.4s %ux%u crop %ux%u rate=%ukbps rc=%u usage=%u\n",
            (int)qsts, (const char*)&q.mfx.FrameInfo.FourCC, q.mfx.FrameInfo.Width,
            q.mfx.FrameInfo.Height, q.mfx.FrameInfo.CropW, q.mfx.FrameInfo.CropH,
            q.mfx.TargetKbps, q.mfx.RateControlMethod, q.mfx.TargetUsage);
    mfxStatus sts = v.EncInit(v.session, &enc_par);
    if (sts != MFX_ERR_NONE) { fprintf(stderr, "EncInit %d\n", (int)sts); return 1; }

    mfxFrameSurface1 surf{};
    std::vector<std::uint8_t> backing;
    fill_surface_nv12(surf, yuv, w, h, backing);

    std::vector<mfxU8> bs_buf(64 * 1024 * 1024);
    mfxBitstream bs{};
    bs.Data = bs_buf.data();
    bs.MaxLength = (mfxU32)bs_buf.size();
    bs.DataFlag = MFX_BITSTREAM_COMPLETE_FRAME;

    mfxSyncPoint syncp = nullptr;
    for (int spins = 0; spins < 100; ++spins) {
        sts = v.EncodeFrameAsync(v.session, nullptr, &surf, &bs, &syncp);
        if (sts == MFX_WRN_DEVICE_BUSY) { Sleep(1); continue; }
        break;
    }
    if (sts != MFX_ERR_NONE && sts != MFX_ERR_NONE_PARTIAL_OUTPUT) {
        fprintf(stderr, "EncodeFrameAsync %d\n", (int)sts);
        return 1;
    }
    // 帧尾 EOS
    if (syncp) v.SyncOperation(v.session, syncp, 1000);
    for (int spins = 0; spins < 100; ++spins) {
        sts = v.EncodeFrameAsync(v.session, nullptr, nullptr, &bs, &syncp);
        if (sts == MFX_WRN_DEVICE_BUSY) { Sleep(1); continue; }
        if (syncp) v.SyncOperation(v.session, syncp, 1000);
        break;
    }
    size_t enc_size = bs.DataLength;
    std::vector<std::uint8_t> stream(bs.Data + bs.DataOffset, bs.Data + bs.DataOffset + enc_size);
    printf("encoded %zuB @%ukbps\n", enc_size, target_kbps);
    v.EncClose(v.session);

    // ---- 解码 ----
    mfxVideoParam dec_par{};
    dec_par.mfx.CodecId = MFX_CODEC_AVC;
    dec_par.IOPattern = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    dec_par.AsyncDepth = 1;

    mfxBitstream dbs{};
    dbs.Data = stream.data();
    dbs.DataLength = (mfxU32)stream.size();
    dbs.MaxLength = (mfxU32)stream.size();
    dbs.DataFlag = MFX_BITSTREAM_COMPLETE_FRAME;

    sts = v.DecodeHeader(v.session, &dbs, &dec_par);
    if (sts < 0) { fprintf(stderr, "DecodeHeader %d\n", (int)sts); return 1; }
    sts = v.DecInit(v.session, &dec_par);
    if (sts != MFX_ERR_NONE) { fprintf(stderr, "DecInit %d\n", (int)sts); return 1; }

    mfxFrameSurface1* out_surf = nullptr;
    mfxSyncPoint dsync = nullptr;
    std::vector<std::uint8_t> decoded;
    for (int spins = 0; spins < 200; ++spins) {
        sts = v.DecodeFrameAsync(v.session, &dbs, nullptr, &out_surf, &dsync);
        if (sts == MFX_WRN_DEVICE_BUSY) { Sleep(1); continue; }
        if (sts == MFX_ERR_MORE_DATA) break;
        if (sts < 0) { fprintf(stderr, "DecodeFrameAsync %d\n", (int)sts); return 1; }
        if (dsync) {
            v.SyncOperation(v.session, dsync, 1000);
            if (out_surf) {
                // Map 回读（显存 surface）
                if (out_surf->FrameInterface) {
                    out_surf->FrameInterface->Map(out_surf, MFX_MAP_READ);
                    surface_to_i420(out_surf, w, h, decoded);
                    out_surf->FrameInterface->Unmap(out_surf);
                    out_surf->FrameInterface->Release(out_surf);
                } else {
                    surface_to_i420(out_surf, w, h, decoded);
                }
                break;
            }
        }
    }
    if (decoded.empty()) { fprintf(stderr, "decode: no frame out\n"); return 1; }
    write_all(argv[5], decoded);
    printf("decoded -> %s (%zuB)\n", argv[5], decoded.size());

    v.DecClose(v.session);
    v.Close(v.session);
    v.Unload(v.loader);
    FreeLibrary(v.dll);
    return 0;
}
