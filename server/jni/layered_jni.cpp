// com.tightcast.server.Layered 的 JNI 实现：基础层重建帧与原始 I420 帧的残差
// 熵编码（协议 docs/protocol.md §3.4），逻辑在 shared/layered（纯 C++ 可单测）。
#include <jni.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "layered/camouflage.h"
#include "layered/entropy.h"
#include "layered/residual.h"

namespace {

inline std::uint8_t* direct_addr(JNIEnv* env, jobject buf) {
    return static_cast<std::uint8_t*>(env->GetDirectBufferAddress(buf));
}

}  // namespace

// 残差 sym 平面原样输出（实验/调试用；compact recon 由 nativeCopyPlanes 先拷好）
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_tightcast_server_Layered_nativeComputeSym(
        JNIEnv* env, jclass /*cls*/, jobject orig, jint w, jint h,
        jobject ry, jint y_stride, jint y_ps,
        jobject ru, jint u_stride, jint u_ps,
        jobject rv, jint v_stride, jint v_ps) {
    const std::uint8_t* o = direct_addr(env, orig);
    const std::uint8_t* py = direct_addr(env, ry);
    const std::uint8_t* pu = direct_addr(env, ru);
    const std::uint8_t* pv = direct_addr(env, rv);
    if (o == nullptr || py == nullptr || pu == nullptr || pv == nullptr) return nullptr;
    if (w <= 0 || h <= 0 || (w & 1) || (h & 1)) return nullptr;
    if (env->GetDirectBufferCapacity(orig) < (jlong)layered::frame_bytes(w, h)) return nullptr;

    std::vector<std::uint8_t> sym(layered::frame_bytes(w, h));
    layered::compute_residual(o, w, h,
                              py, y_stride, y_ps,
                              pu, u_stride, u_ps,
                              pv, v_stride, v_ps, sym.data());
    jbyteArray arr = env->NewByteArray((jsize)sym.size());
    if (arr == nullptr) return nullptr;
    env->SetByteArrayRegion(arr, 0, (jsize)sym.size(),
                            reinterpret_cast<const jbyte*>(sym.data()));
    return arr;
}

// 迷彩变换（方案A）：inverse=false 正向模 256 差分；true 前缀和逆变换
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_tightcast_server_Layered_nativeCamouflage(
        JNIEnv* env, jclass /*cls*/, jbyteArray symArr, jint w, jint h, jboolean inverse) {
    jsize len = env->GetArrayLength(symArr);
    if (len != (jsize)layered::frame_bytes(w, h)) return nullptr;
    std::vector<std::uint8_t> in((std::size_t)len);
    env->GetByteArrayRegion(symArr, 0, len, reinterpret_cast<jbyte*>(in.data()));
    std::vector<std::uint8_t> out((std::size_t)len);
    if (inverse == JNI_TRUE) {
        layered::camouflage_inverse(in.data(), w, h, out.data());
    } else {
        layered::camouflage_forward(in.data(), w, h, out.data());
    }
    jbyteArray arr = env->NewByteArray(len);
    if (arr == nullptr) return nullptr;
    env->SetByteArrayRegion(arr, 0, len, reinterpret_cast<const jbyte*>(out.data()));
    return arr;
}

// 解码输出 Image 三平面 → 紧凑 planar I420 拷贝（BaseDecoder 回调线程上执行，
// ~2ms，拷完即释放解码输出缓冲；残差计算/熵编码在增强工作线程做）。
extern "C" JNIEXPORT void JNICALL
Java_com_tightcast_server_Layered_nativeCopyPlanes(
        JNIEnv* env, jclass /*cls*/,
        jobject ry, jint y_stride, jint y_ps,
        jobject ru, jint u_stride, jint u_ps,
        jobject rv, jint v_stride, jint v_ps,
        jobject dst, jint w, jint h) {
    const std::uint8_t* py = direct_addr(env, ry);
    const std::uint8_t* pu = direct_addr(env, ru);
    const std::uint8_t* pv = direct_addr(env, rv);
    std::uint8_t* d = direct_addr(env, dst);
    if (py == nullptr || pu == nullptr || pv == nullptr || d == nullptr) return;
    if (w <= 0 || h <= 0 || (w & 1) || (h & 1)) return;
    if (env->GetDirectBufferCapacity(dst) < (jlong)layered::frame_bytes(w, h)) return;
    jlong yc = env->GetDirectBufferCapacity(ry);
    jlong uc = env->GetDirectBufferCapacity(ru);
    jlong vc = env->GetDirectBufferCapacity(rv);
    if (yc < (int64_t)y_stride * (h - 1) + (int64_t)w * y_ps
            || uc < (int64_t)u_stride * (h / 2 - 1) + (int64_t)(w / 2) * u_ps
            || vc < (int64_t)v_stride * (h / 2 - 1) + (int64_t)(w / 2) * v_ps) {
        return;
    }
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* s = py + (std::size_t)y * y_stride;
        std::uint8_t* o = d + (std::size_t)y * w;
        if (y_ps == 1) {
            std::memcpy(o, s, (std::size_t)w);
        } else {
            for (int x = 0; x < w; ++x) o[x] = s[(std::size_t)x * y_ps];
        }
    }
    const int cw = w / 2, ch = h / 2;
    std::uint8_t* du = d + (std::size_t)w * h;
    std::uint8_t* dv = du + (std::size_t)cw * ch;
    for (int y = 0; y < ch; ++y) {
        const std::uint8_t* su = pu + (std::size_t)y * u_stride;
        const std::uint8_t* sv = pv + (std::size_t)y * v_stride;
        std::uint8_t* ou = du + (std::size_t)y * cw;
        std::uint8_t* ov = dv + (std::size_t)y * cw;
        for (int x = 0; x < cw; ++x) {
            ou[x] = su[(std::size_t)x * u_ps];
            ov[x] = sv[(std::size_t)x * v_ps];
        }
    }
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_tightcast_server_Layered_nativeComputeEnhancement(
        JNIEnv* env, jclass /*cls*/, jobject orig, jint w, jint h,
        jobject ry, jint y_stride, jint y_ps,
        jobject ru, jint u_stride, jint u_ps,
        jobject rv, jint v_stride, jint v_ps, jlong max_bytes) {
    const std::uint8_t* o = direct_addr(env, orig);
    const std::uint8_t* py = direct_addr(env, ry);
    const std::uint8_t* pu = direct_addr(env, ru);
    const std::uint8_t* pv = direct_addr(env, rv);
    if (o == nullptr || py == nullptr || pu == nullptr || pv == nullptr) return nullptr;
    if (w <= 0 || h <= 0 || (w & 1) || (h & 1)) return nullptr;
    // 容量防御（recon 平面来自解码器，布局异常时宁可丢帧不可崩溃）
    jlong oc = env->GetDirectBufferCapacity(orig);
    jlong yc = env->GetDirectBufferCapacity(ry);
    jlong uc = env->GetDirectBufferCapacity(ru);
    jlong vc = env->GetDirectBufferCapacity(rv);
    int64_t y_need = (int64_t)y_stride * (h - 1) + (int64_t)w * y_ps;
    int64_t c_need = (int64_t)u_stride * (h / 2 - 1) + (int64_t)(w / 2) * u_ps;
    int64_t c_need_v = (int64_t)v_stride * (h / 2 - 1) + (int64_t)(w / 2) * v_ps;
    if (oc < (int64_t)layered::frame_bytes(w, h) || yc < y_need
            || uc < c_need || vc < c_need_v) {
        return nullptr;
    }

    // 线程局部暂存，避免每帧 2.5MB 堆分配（JNI 调用固定来自 BaseDecoder 线程）
    static thread_local std::vector<std::uint8_t> sym;
    sym.resize(layered::frame_bytes(w, h));
    layered::compute_residual(o, w, h,
                              py, y_stride, y_ps,
                              pu, u_stride, u_ps,
                              pv, v_stride, v_ps, sym.data());

    std::vector<std::uint8_t> coded;
    if (!layered::encode_enhancement(sym.data(), w, h,
                                     (std::size_t)(max_bytes > 0 ? max_bytes : 512 * 1024),
                                     coded)) {
        return nullptr;  // 尺寸护栏：该帧增强弃发
    }
    jbyteArray arr = env->NewByteArray((jsize)coded.size());
    if (arr == nullptr) return nullptr;
    env->SetByteArrayRegion(arr, 0, (jsize)coded.size(),
                            reinterpret_cast<const jbyte*>(coded.data()));
    return arr;
}
