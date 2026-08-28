// com.tightcast.client.NativeClient 的 JNI 实现：转发到 C++ 核心（client.cpp）。
#include <jni.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "client.h"

namespace {

JavaVM* g_vm = nullptr;
jobject g_dec_base = nullptr;   // VideoDec layer=0
jobject g_dec_enh = nullptr;    // VideoDec layer=1
jmethodID g_dec_feed = nullptr; // feed([BJI)Z
jclass g_log2file_cls = nullptr;  // com.tightcast.client.Log2File
jmethodID g_log2file = nullptr;   // Log2File.log(String)

JNIEnv* get_env(bool* attached) {
    *attached = false;
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    if (g_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
        *attached = true;
        return env;
    }
    return nullptr;
}

// native 解码/增强线程 → Java VideoDec.feed（锁序：无锁调用，tight/Java 两侧各自快照）
bool java_dec_feed(jobject dec, const std::uint8_t* au, std::size_t size,
                   std::uint64_t pts_ms, int flags) {
    if (dec == nullptr || g_dec_feed == nullptr) return false;
    bool attached;
    JNIEnv* env = get_env(&attached);
    if (env == nullptr) return false;
    jbyteArray arr = env->NewByteArray((jsize)size);
    if (arr == nullptr) {
        if (attached) g_vm->DetachCurrentThread();
        return false;
    }
    env->SetByteArrayRegion(arr, 0, (jsize)size, reinterpret_cast<const jbyte*>(au));
    jboolean ok = env->CallBooleanMethod(dec, g_dec_feed, arr, (jlong)pts_ms, (jint)flags);
    env->DeleteLocalRef(arr);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (attached) g_vm->DetachCurrentThread();
    return ok == JNI_TRUE;
}

}  // namespace

// client.cpp 的 CLOG 落点：native 门控/配对事件写 Log2File（华为 logcat 抑制）
namespace ClientLog {
void hook(const char* msg) {
    if (g_log2file == nullptr || g_log2file_cls == nullptr || g_vm == nullptr) return;
    bool attached;
    JNIEnv* env = get_env(&attached);
    if (env == nullptr) return;
    jstring s = env->NewStringUTF(msg);
    env->CallStaticVoidMethod(g_log2file_cls, g_log2file, s);
    env->DeleteLocalRef(s);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (attached) g_vm->DetachCurrentThread();
}
}  // namespace ClientLog

extern "C" {

jint JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
        jclass cls = env->FindClass("com/tightcast/client/Log2File");
        if (cls != nullptr) {
            g_log2file_cls = (jclass)env->NewGlobalRef(cls);
            g_log2file = env->GetStaticMethodID(cls, "log", "(Ljava/lang/String;)V");
            env->DeleteLocalRef(cls);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    return JNI_VERSION_1_6;
}

// 注册 Java 解码器（layer 0=基础 1=增强）；注册后 native 走 Java MediaCodec 路径
JNIEXPORT void JNICALL
Java_com_tightcast_client_NativeClient_nativeSetVideoDecs(JNIEnv* env, jclass,
                                                          jobject base, jobject enh) {
    if (g_dec_base) env->DeleteGlobalRef(g_dec_base);
    if (g_dec_enh) env->DeleteGlobalRef(g_dec_enh);
    g_dec_base = env->NewGlobalRef(base);
    g_dec_enh = env->NewGlobalRef(enh);
    jclass cls = env->GetObjectClass(base);
    g_dec_feed = env->GetMethodID(cls, "feed", "([BJI)Z");
    Client::instance().set_java_decoder(0, {[&](const std::uint8_t* au, std::size_t n,
                                               std::uint64_t pts, int flags) {
        return java_dec_feed(g_dec_base, au, n, pts, flags);
    }});
    Client::instance().set_java_decoder(1, {[&](const std::uint8_t* au, std::size_t n,
                                               std::uint64_t pts, int flags) {
        return java_dec_feed(g_dec_enh, au, n, pts, flags);
    }});
}

// Java 解码输出回调（VideoDec codec 线程）：平面切片已含 crop 偏移
JNIEXPORT void JNICALL
Java_com_tightcast_client_NativeClient_nativeOnDecodedFrame(
        JNIEnv* env, jclass, jint layer, jlong ptsMs, jint flags,
        jobject y, jint yStride, jint yPs,
        jobject u, jint uStride, jint uPs,
        jobject v, jint vStride, jint vPs,
        jint w, jint h) {
    const std::uint8_t* py = static_cast<std::uint8_t*>(env->GetDirectBufferAddress(y));
    const std::uint8_t* pu = static_cast<std::uint8_t*>(env->GetDirectBufferAddress(u));
    const std::uint8_t* pv = static_cast<std::uint8_t*>(env->GetDirectBufferAddress(v));
    if (!py || !pu || !pv) return;
    Client::instance().on_java_decoded(layer, (std::uint64_t)ptsMs, flags,
                                       py, yStride, yPs, pu, uStride, uPs,
                                       pv, vStride, vPs, w, h);
}



JNIEXPORT jboolean JNICALL
Java_com_tightcast_client_NativeClient_nativeStart(JNIEnv* env, jclass,
                                                   jstring host, jint port, jstring token) {
    const char* h = env->GetStringUTFChars(host, nullptr);
    const char* t = env->GetStringUTFChars(token, nullptr);
    bool ok = Client::instance().start(h, (std::uint16_t)port, t);
    env->ReleaseStringUTFChars(host, h);
    env->ReleaseStringUTFChars(token, t);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_tightcast_client_NativeClient_nativeStop(JNIEnv*, jclass) {
    Client::instance().stop();
}

JNIEXPORT jboolean JNICALL
Java_com_tightcast_client_NativeClient_nativeIsOnline(JNIEnv*, jclass) {
    return Client::instance().online() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_tightcast_client_NativeClient_nativeSetMode(JNIEnv*, jclass, jint mode) {
    Client::instance().send_set_format((std::uint8_t)mode);
}

JNIEXPORT void JNICALL
Java_com_tightcast_client_NativeClient_nativeSetEnhWait(JNIEnv*, jclass, jint ms) {
    Client::instance().set_grace_ms(ms);
}

JNIEXPORT void JNICALL
Java_com_tightcast_client_NativeClient_nativeTouch(JNIEnv*, jclass,
                                                   jint action, jfloat x, jfloat y) {
    Client::instance().send_touch((std::uint8_t)action, x, y);
}

JNIEXPORT void JNICALL
Java_com_tightcast_client_NativeClient_nativeKey(JNIEnv*, jclass, jint action, jint keycode) {
    Client::instance().send_key((std::uint8_t)action, keycode);
}

JNIEXPORT void JNICALL
Java_com_tightcast_client_NativeClient_nativeText(JNIEnv* env, jclass, jstring text) {
    const char* s = env->GetStringUTFChars(text, nullptr);
    Client::instance().send_text(s);
    env->ReleaseStringUTFChars(text, s);
}

JNIEXPORT void JNICALL
Java_com_tightcast_client_NativeClient_nativeScroll(JNIEnv*, jclass,
                                                    jfloat x, jfloat y, jfloat dy) {
    Client::instance().send_scroll(x, y, dy);
}

JNIEXPORT void JNICALL
Java_com_tightcast_client_NativeClient_nativePcm(JNIEnv* env, jclass,
                                                 jbyteArray pcm, jint len) {
    jbyte* data = env->GetByteArrayElements(pcm, nullptr);
    if (data) {
        Client::instance().send_audio(reinterpret_cast<const std::uint8_t*>(data),
                                      (std::size_t)len);
        env->ReleaseByteArrayElements(pcm, data, JNI_ABORT);
    }
}

JNIEXPORT jintArray JNICALL
Java_com_tightcast_client_NativeClient_nativeVideoSize(JNIEnv* env, jclass) {
    int w = 0, h = 0;
    Client::instance().renderer().video_size(w, h);
    jintArray arr = env->NewIntArray(2);
    if (arr) {
        jint v[2] = {w, h};
        env->SetIntArrayRegion(arr, 0, 2, v);
    }
    return arr;
}

// 上屏统计行（UI 状态条）：shown/composited/enh_idr/got_idr/jank‰/grace + 解码器状态
JNIEXPORT jstring JNICALL
Java_com_tightcast_client_NativeClient_nativeStatsLine(JNIEnv* env, jclass) {
    std::uint64_t s[5];
    Client::instance().stats(s);
    std::string line = "shown=" + std::to_string(s[0])
            + " composited=" + std::to_string(s[1])
            + " enh_idr=" + std::to_string(s[2])
            + " idr=" + std::to_string(Client::instance().got_idr() ? 1 : 0)
            + " jank=" + std::to_string(s[3] / 10) + "." + std::to_string(s[3] % 10) + "%"
            + " grace=" + std::to_string(s[4]) + "ms"
            + " " + g_dec_status;
    return env->NewStringUTF(line.c_str());
}

// ---- GL 线程（GLSurfaceView.Renderer 回调）----

JNIEXPORT void JNICALL
Java_com_tightcast_client_NativeClient_nativeSurfaceCreated(JNIEnv*, jclass) {
    Client::instance().renderer().on_surface_created();
}

JNIEXPORT void JNICALL
Java_com_tightcast_client_NativeClient_nativeSurfaceChanged(JNIEnv*, jclass,
                                                            jint w, jint h) {
    Client::instance().renderer().on_surface_changed(w, h);
}

JNIEXPORT void JNICALL
Java_com_tightcast_client_NativeClient_nativeDrawFrame(JNIEnv*, jclass) {
    Client::instance().renderer().on_draw_frame();
}

}  // extern "C"
