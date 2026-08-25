// com.tightcast.server.Repack 的 JNI 实现，逻辑在 repack_core.cpp（纯 C++ 可单测）。
#include <jni.h>

#include <cstdint>
#include <vector>

#include "repack_core.h"

namespace {

inline std::uint8_t* direct_addr(JNIEnv* env, jobject buf) {
    // Java 侧保证传入 position=0 的 slice()，返回地址即有效区起始
    return static_cast<std::uint8_t*>(env->GetDirectBufferAddress(buf));
}

}  // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_tightcast_server_Repack_nativeRepackDirect(
        JNIEnv* env, jclass /*cls*/, jobject rgba, jint w, jint h,
        jint row_stride, jint pixel_stride, jobject dst, jboolean ycocg) {
    const std::uint8_t* src = direct_addr(env, rgba);
    std::uint8_t* out = direct_addr(env, dst);
    if (src == nullptr || out == nullptr) return;
    // 容量防御：recreate/竞态下图像尺寸或 dst 缓冲可能与 (w,h) 不符，
    // 越界即 SIGSEGV（真机 tombstone 实录）——宁可丢帧不可崩溃
    jlong src_cap = env->GetDirectBufferCapacity(rgba);
    jlong dst_cap = env->GetDirectBufferCapacity(dst);
    int64_t src_need = (int64_t)row_stride * (h - 1) + (int64_t)w * pixel_stride;
    int64_t dst_need = (int64_t)w * 2 * h * 3 / 2;
    if (w <= 0 || h <= 0 || src_cap < src_need || dst_cap < dst_need) return;
    if (ycocg == JNI_TRUE) {
        repack::repack_rgba_to_dual_i420_ycocg(src, w, h, row_stride, pixel_stride, out);
    } else {
        repack::repack_rgba_to_dual_i420(src, w, h, row_stride, pixel_stride, out);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_tightcast_server_Repack_nativeRepackArray(
        JNIEnv* env, jclass /*cls*/, jbyteArray rgba, jint offset, jint w, jint h,
        jint row_stride, jint pixel_stride, jobject dst) {
    std::uint8_t* out = direct_addr(env, dst);
    if (out == nullptr) return;
    jsize len = env->GetArrayLength(rgba);
    if (len <= 0) return;
    // 容量防御（同 direct 版）
    int64_t src_need = (int64_t)offset + (int64_t)row_stride * (h - 1)
                     + (int64_t)w * pixel_stride;
    jlong dst_cap = env->GetDirectBufferCapacity(dst);
    int64_t dst_need = (int64_t)w * 2 * h * 3 / 2;
    if (w <= 0 || h <= 0 || (int64_t)len < src_need || dst_cap < dst_need) return;
    std::vector<std::uint8_t> tmp(static_cast<size_t>(len));
    env->GetByteArrayRegion(rgba, 0, len, reinterpret_cast<jbyte*>(tmp.data()));
    if (env->ExceptionCheck()) return;
    repack::repack_rgba_to_dual_i420(tmp.data() + offset, w, h, row_stride, pixel_stride, out);
}

extern "C" JNIEXPORT void JNICALL
Java_com_tightcast_server_Repack_nativeRepackArrayYcocg(
        JNIEnv* env, jclass /*cls*/, jbyteArray rgba, jint offset, jint w, jint h,
        jint row_stride, jint pixel_stride, jobject dst) {
    std::uint8_t* out = direct_addr(env, dst);
    if (out == nullptr) return;
    jsize len = env->GetArrayLength(rgba);
    if (len <= 0) return;
    // 容量防御（同 nativeRepackArray）
    int64_t src_need = (int64_t)offset + (int64_t)row_stride * (h - 1)
                     + (int64_t)w * pixel_stride;
    jlong dst_cap = env->GetDirectBufferCapacity(dst);
    int64_t dst_need = (int64_t)w * 2 * h * 3 / 2;
    if (w <= 0 || h <= 0 || (int64_t)len < src_need || dst_cap < dst_need) return;
    std::vector<std::uint8_t> tmp(static_cast<size_t>(len));
    env->GetByteArrayRegion(rgba, 0, len, reinterpret_cast<jbyte*>(tmp.data()));
    if (env->ExceptionCheck()) return;
    repack::repack_rgba_to_dual_i420_ycocg(tmp.data() + offset, w, h,
                                           row_stride, pixel_stride, out);
}

extern "C" JNIEXPORT void JNICALL
Java_com_tightcast_server_Repack_nativeRgbaToI420(
        JNIEnv* env, jclass /*cls*/, jbyteArray rgba, jint offset, jint w, jint h,
        jint row_stride, jint pixel_stride, jobject dst) {
    std::uint8_t* out = direct_addr(env, dst);
    if (out == nullptr) return;
    jsize len = env->GetArrayLength(rgba);
    if (len <= 0) return;
    // 容量防御（同 nativeRepackArray）
    int64_t src_need = (int64_t)offset + (int64_t)row_stride * (h - 1)
                     + (int64_t)w * pixel_stride;
    jlong dst_cap = env->GetDirectBufferCapacity(dst);
    int64_t dst_need = (int64_t)w * h * 3 / 2;
    if (w <= 0 || h <= 0 || (int64_t)len < src_need || dst_cap < dst_need) return;
    std::vector<std::uint8_t> tmp(static_cast<size_t>(len));
    env->GetByteArrayRegion(rgba, 0, len, reinterpret_cast<jbyte*>(tmp.data()));
    if (env->ExceptionCheck()) return;
    repack::rgba_to_i420(tmp.data() + offset, w, h, row_stride, pixel_stride, out);
}

extern "C" JNIEXPORT void JNICALL
Java_com_tightcast_server_Repack_nativeFillInputImage(
        JNIEnv* env, jclass /*cls*/, jobject src, jint enc_w, jint enc_h,
        jobject y_buf, jint y_row_stride, jint y_pixel_stride,
        jobject u_buf, jint u_row_stride, jint u_pixel_stride,
        jobject v_buf, jint v_row_stride, jint v_pixel_stride) {
    const std::uint8_t* s = direct_addr(env, src);
    std::uint8_t* yb = direct_addr(env, y_buf);
    std::uint8_t* ub = direct_addr(env, u_buf);
    std::uint8_t* vb = direct_addr(env, v_buf);
    if (s == nullptr || yb == nullptr || ub == nullptr || vb == nullptr) return;
    repack::fill_input_image(s, enc_w, enc_h,
                             yb, y_row_stride, y_pixel_stride,
                             ub, u_row_stride, u_pixel_stride,
                             vb, v_row_stride, v_pixel_stride);
}
