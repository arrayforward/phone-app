#include "decoder.h"

#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/native_window.h>

#include <android/log.h>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <thread>

#define TAG "tightcast-dec"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

std::string g_dec_status;

namespace {

// 诊断状态（华为 ROM 抑制应用 logcat 时经 UI 状态条可见）+ logcat 双写
void dec_status(const char* fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_dec_status = buf;
    LOGE("%s", buf);
}

// ---- Annex-B 工具：起始码扫描 ----
// 返回 [nal_begin, nal_end) 区间序列（nal_begin 指向 NAL 头字节，即起始码之后）
struct NalRange { const std::uint8_t* begin; const std::uint8_t* end; };

std::vector<NalRange> split_annexb(const std::uint8_t* data, std::size_t size) {
    std::vector<NalRange> out;
    std::size_t i = 0;
    auto start_code_len = [&](std::size_t p) -> int {
        if (p + 3 <= size && data[p] == 0 && data[p + 1] == 0 && data[p + 2] == 1) return 3;
        if (p + 4 <= size && data[p] == 0 && data[p + 1] == 0 && data[p + 2] == 0 && data[p + 3] == 1) return 4;
        return 0;
    };
    while (i < size) {
        int sc = start_code_len(i);
        if (!sc) { ++i; continue; }
        std::size_t nb = i + sc;
        std::size_t ne = size;
        for (std::size_t j = nb; j + 3 <= size; ++j) {
            if (start_code_len(j)) { ne = j; break; }
        }
        if (nb < ne) out.push_back({data + nb, data + ne});
        i = ne;
    }
    return out;
}

// ---- Exp-Golomb 位读（SPS 解析，含防竞争字节 00 00 03 → 00 00 去除）----
class BitReaderEBSP {
public:
    BitReaderEBSP(const std::uint8_t* data, std::size_t size) {
        m_rbsp.reserve(size);
        for (std::size_t i = 0; i < size; ++i) {
            // 00 00 03 xx → 00 00 xx
            if (i >= 2 && m_rbsp.size() >= 2
                    && m_rbsp[m_rbsp.size() - 1] == 0 && m_rbsp[m_rbsp.size() - 2] == 0
                    && data[i] == 3) {
                continue;
            }
            m_rbsp.push_back(data[i]);
        }
    }
    int u1() {
        if (m_pos >= m_rbsp.size() * 8) return 0;
        int b = (m_rbsp[m_pos >> 3] >> (7 - (m_pos & 7))) & 1;
        ++m_pos;
        return b;
    }
    std::uint32_t ue() {
        int zeros = 0;
        while (u1() == 0 && zeros < 32) ++zeros;
        if (zeros == 0) return 0;
        std::uint32_t suffix = 0;
        for (int i = 0; i < zeros; ++i) suffix = (suffix << 1) | (std::uint32_t)u1();
        return ((1u << zeros) - 1) + suffix;
    }
    std::int32_t se() {
        std::uint32_t v = ue();
        return (v & 1) ? (std::int32_t)((v + 1) / 2) : -(std::int32_t)(v / 2);
    }
private:
    std::vector<std::uint8_t> m_rbsp;
    std::size_t m_pos = 0;
};

// 解析 SPS（不含 NAL 头）得显示尺寸（含 frame_crop）。返回 false = 解析失败
// （如 scaling matrix present——MediaCodec 编码器一般不产出，调用方走回退）
bool parse_sps_dims(const std::uint8_t* sps, std::size_t size, int& w, int& h) {
    if (size < 4) return false;
    BitReaderEBSP br(sps, size);
    int profile = 0;
    for (int i = 0; i < 8; ++i) profile = (profile << 1) | br.u1();  // profile_idc
    for (int i = 0; i < 8; ++i) br.u1();   // constraint flags(6) + reserved(2)
    for (int i = 0; i < 8; ++i) br.u1();   // level_idc
    br.ue();  // seq_parameter_set_id
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244
            || profile == 44 || profile == 83 || profile == 86 || profile == 118
            || profile == 128 || profile == 138 || profile == 139 || profile == 134
            || profile == 135) {
        std::uint32_t chroma = br.ue();
        if (chroma == 3) br.u1();  // separate_colour_plane_flag
        br.ue();  // bit_depth_luma_minus8
        br.ue();  // bit_depth_chroma_minus8
        br.u1();  // qpprime_y_zero_transform_bypass_flag
        if (br.u1()) return false;  // seq_scaling_matrix_present：不解析，走回退
    }
    br.ue();  // log2_max_frame_num_minus4
    std::uint32_t poc_type = br.ue();
    if (poc_type == 0) {
        br.ue();  // log2_max_pic_order_cnt_lsb_minus4
    } else if (poc_type == 1) {
        br.u1();  // delta_pic_order_always_zero_flag
        br.se();  // offset_for_non_ref_pic
        br.se();  // offset_for_top_to_bottom_field
        std::uint32_t n = br.ue();  // num_ref_frames_in_pic_order_cnt_cycle
        if (n > 255) return false;
        for (std::uint32_t i = 0; i < n; ++i) br.se();
    }
    br.ue();  // max_num_ref_frames
    br.u1();  // gaps_in_frame_num_value_allowed_flag
    std::uint32_t pic_w_mbs = br.ue();      // pic_width_in_mbs_minus1
    std::uint32_t pic_h_map = br.ue();      // pic_height_in_map_units_minus1
    if (pic_w_mbs > 1023 || pic_h_map > 1023) return false;
    int frame_only = br.u1();               // frame_mbs_only_flag
    if (!frame_only) br.u1();               // mb_adaptive_frame_field_flag
    br.u1();                                // direct_8x8_inference_flag
    int cw = (int)(pic_w_mbs + 1) * 16;
    int ch = (int)(pic_h_map + 1) * 16 * (frame_only ? 1 : 2);
    if (br.u1()) {  // frame_cropping_flag
        std::uint32_t left = br.ue(), right = br.ue(), top = br.ue(), bottom = br.ue();
        // 4:2:0 裁剪单位：横向 2、纵向 2（frame_mbs_only）
        int sub_x = 2, sub_y = frame_only ? 2 : 4;
        cw -= (int)((left + right) * sub_x);
        ch -= (int)((top + bottom) * sub_y);
    }
    if (cw <= 0 || ch <= 0) return false;
    w = cw;
    h = ch;
    return true;
}

}  // namespace

struct H264Decoder::Impl {
    AMediaCodec* codec = nullptr;
    AImageReader* reader = nullptr;
    ANativeWindow* window = nullptr;
    int cfg_w = 0;
    int cfg_h = 0;

    void destroy() {
        if (codec) {
            AMediaCodec_stop(codec);
            AMediaCodec_delete(codec);
            codec = nullptr;
        }
        if (reader) {
            AImageReader_delete(reader);  // 连带释放其 window
            reader = nullptr;
            window = nullptr;
        }
        cfg_w = cfg_h = 0;
    }
};

H264Decoder::~H264Decoder() { shutdown(); }

void H264Decoder::shutdown() {
    if (m) {
        m->destroy();
        delete m;
        m = nullptr;
    }
}

bool H264Decoder::ensure_configured(const std::uint8_t* au, std::size_t size) {
    // 从 AU 提取 SPS/PPS（csd-0/csd-1）并解析显示尺寸
    auto nals = split_annexb(au, size);
    const NalRange* sps = nullptr;
    const NalRange* pps = nullptr;
    for (const auto& n : nals) {
        int type = n.begin[0] & 0x1F;
        if (type == 7 && !sps) sps = &n;
        else if (type == 8 && !pps) pps = &n;
    }
    if (!sps || !pps) {
        dec_status("no SPS/PPS in IDR AU (%zu nals)", nals.size());
        return false;
    }
    int w = 0, h = 0;
    if (!parse_sps_dims(sps->begin + 1, (std::size_t)(sps->end - sps->begin - 1), w, h)) {
        dec_status("SPS parse failed");
        return false;
    }
    if (m && m->cfg_w == w && m->cfg_h == h) return true;  // 尺寸未变，沿用
    shutdown();  // 尺寸变化（旋转/格式切换）→ 重建解码器
    m = new Impl();

    media_status_t st = AImageReader_new(w, h, AIMAGE_FORMAT_YUV_420_888,
                                         4 /*maxImages*/, &m->reader);
    if (st != AMEDIA_OK || !m->reader) {
        dec_status("AImageReader_new failed: %d", (int)st);
        shutdown();
        return false;
    }
    st = AImageReader_getWindow(m->reader, &m->window);
    if (st != AMEDIA_OK || !m->window) {
        dec_status("AImageReader_getWindow failed: %d", (int)st);
        shutdown();
        return false;
    }
    m->codec = AMediaCodec_createDecoderByType("video/avc");
    if (!m->codec) {
        dec_status("createDecoderByType failed");
        shutdown();
        return false;
    }
    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, w);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, h);
    AMediaFormat_setInt32(fmt, "low-latency", 1);
    AMediaFormat_setBuffer(fmt, "csd-0", sps->begin, (std::size_t)(sps->end - sps->begin));
    AMediaFormat_setBuffer(fmt, "csd-1", pps->begin, (std::size_t)(pps->end - pps->begin));
    st = AMediaCodec_configure(m->codec, fmt, m->window, nullptr, 0);
    AMediaFormat_delete(fmt);
    if (st != AMEDIA_OK) {
        dec_status("AMediaCodec_configure failed: %d (%dx%d)", (int)st, w, h);
        shutdown();
        return false;
    }
    st = AMediaCodec_start(m->codec);
    if (st != AMEDIA_OK) {
        dec_status("AMediaCodec_start failed: %d", (int)st);
        shutdown();
        return false;
    }
    m->cfg_w = w;
    m->cfg_h = h;
    dec_status("decoder configured %dx%d", w, h);
    return true;
}

bool H264Decoder::decode(const std::uint8_t* data, std::size_t size, bool idr,
                         VideoFrame& out) {
    if (!m) {
        if (!idr) return false;  // 首个 IDR 之前不送解码器
        if (!ensure_configured(data, size)) return false;
    } else if (idr) {
        // IDR 上检查尺寸是否变化（旋转/格式切换后首帧必为 IDR）
        auto nals = split_annexb(data, size);
        for (const auto& n : nals) {
            if ((n.begin[0] & 0x1F) == 7) {
                int w = 0, h = 0;
                if (parse_sps_dims(n.begin + 1, (std::size_t)(n.end - n.begin - 1), w, h)
                        && (w != m->cfg_w || h != m->cfg_h)) {
                    LOGI("dims changed %dx%d -> %dx%d, reconfigure", m->cfg_w, m->cfg_h, w, h);
                    if (!ensure_configured(data, size)) return false;
                }
                break;
            }
        }
    }

    // 喂输入（短超时重试；拿不到输入缓冲 = 解码器积压，按丢帧处理）
    ssize_t in_idx = -1;
    for (int tries = 0; tries < 8 && in_idx < 0; ++tries) {
        in_idx = AMediaCodec_dequeueInputBuffer(m->codec, 1000);
    }
    if (in_idx < 0) {
        dec_status("no input buffer (decoder backlog)");
        return false;
    }
    std::size_t cap = 0;
    std::uint8_t* in_buf = AMediaCodec_getInputBuffer(m->codec, (std::size_t)in_idx, &cap);
    if (!in_buf || cap < size) {
        dec_status("input buffer too small: %zu < %zu", cap, size);
        AMediaCodec_queueInputBuffer(m->codec, (std::size_t)in_idx, 0, 0, 0, 0);
        return false;
    }
    std::memcpy(in_buf, data, size);
    if (AMediaCodec_queueInputBuffer(m->codec, (std::size_t)in_idx, 0, size, 0, 0)
            != AMEDIA_OK) {
        dec_status("queueInputBuffer failed");
        return false;
    }
    return drain_one(out);
}

bool H264Decoder::drain_one(VideoFrame& out) {
    // 输出经 surface → AImageReader；轮询 acquireNextImage（保序）
    for (int tries = 0; tries < 32; ++tries) {
        AImage* img = nullptr;
        media_status_t st = AImageReader_acquireNextImage(m->reader, &img);
        if (st == AMEDIA_OK && img) {
            int w = 0, h = 0;
            AImage_getWidth(img, &w);
            AImage_getHeight(img, &h);
            int n_planes = 0;
            AImage_getNumberOfPlanes(img, &n_planes);
            if (w <= 0 || h <= 0 || n_planes < 3 || (w & 1) || (h & 1)) {
                dec_status("unexpected image %dx%d planes=%d", w, h, n_planes);
                AImage_delete(img);
                continue;
            }
            std::uint8_t* py = nullptr;
            std::uint8_t* pu = nullptr;
            std::uint8_t* pv = nullptr;
            int y_len = 0, u_len = 0, v_len = 0;
            AImage_getPlaneData(img, 0, &py, &y_len);
            AImage_getPlaneData(img, 1, &pu, &u_len);
            AImage_getPlaneData(img, 2, &pv, &v_len);
            int y_rs = 0, u_rs = 0, v_rs = 0, y_ps = 0, u_ps = 0, v_ps = 0;
            AImage_getPlaneRowStride(img, 0, &y_rs);
            AImage_getPlaneRowStride(img, 1, &u_rs);
            AImage_getPlaneRowStride(img, 2, &v_rs);
            AImage_getPlanePixelStride(img, 0, &y_ps);
            AImage_getPlanePixelStride(img, 1, &u_ps);
            AImage_getPlanePixelStride(img, 2, &v_ps);
            if (!py || !pu || !pv || y_rs <= 0 || u_rs <= 0 || v_rs <= 0) {
                AImage_delete(img);
                continue;
            }
            const int cw = w / 2, ch = h / 2;
            out.width = w;
            out.height = h;
            out.y.resize((std::size_t)w * h);
            out.uv.resize((std::size_t)w * (h / 2));  // NV12 交错：cw*ch*2
            for (int r = 0; r < h; ++r) {
                const std::uint8_t* s = py + (std::size_t)r * y_rs;
                std::uint8_t* d = out.y.data() + (std::size_t)r * w;
                if (y_ps == 1) {
                    std::memcpy(d, s, (std::size_t)w);
                } else {
                    for (int c = 0; c < w; ++c) d[c] = s[(std::size_t)c * y_ps];
                }
            }
            for (int r = 0; r < ch; ++r) {
                const std::uint8_t* su = pu + (std::size_t)r * u_rs;
                const std::uint8_t* sv = pv + (std::size_t)r * v_rs;
                std::uint8_t* d = out.uv.data() + (std::size_t)r * cw * 2;
                for (int c = 0; c < cw; ++c) {
                    d[c * 2] = su[(std::size_t)c * u_ps];
                    d[c * 2 + 1] = sv[(std::size_t)c * v_ps];
                }
            }
            AImage_delete(img);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    dec_status("decode timeout (no image in 32ms)");
    return false;  // 解码超时——按丢帧处理
}
