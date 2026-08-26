// tight JNI 桥：com.tightcast.server.TightBridge 的 native 实现。
// tight server 侧为 LinkRole::Node，bind 0.0.0.0:<port>，等待 PC client(Leaf) 接入。
// 所有 Java 回调从 tight 内部线程触发：AttachCurrentThread 后调用，方法内只做排队/置标志。
#include <jni.h>
#include <android/log.h>

#include <memory>
#include <mutex>
#include <string>
#include <atomic>

#include "tight/tight.hpp"

#define TAG "tightcast-jni"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {

JavaVM* g_vm = nullptr;

std::mutex g_mutex;
std::unique_ptr<tight::TightTransport> g_transport;
jobject g_listener = nullptr;  // TightBridge.Listener 全局引用
jmethodID g_onAudio = nullptr;
jmethodID g_onCommand = nullptr;
jmethodID g_onPeerState = nullptr;
jmethodID g_onVideoCapacity = nullptr;
jmethodID g_onRequestKeyframe = nullptr;
std::string g_peer;   // 发送目标 peer（最新 Online 者优先）
bool g_hasPeer = false;
std::atomic<std::uint64_t> g_videoSendOk{0};
std::atomic<std::uint64_t> g_videoSendFail{0};

JNIEnv* getEnv(bool* didAttach) {
    *didAttach = false;
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
        return env;
    }
    if (g_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
        *didAttach = true;
        return env;
    }
    return nullptr;
}

void releaseEnv(bool didAttach) {
    if (didAttach) g_vm->DetachCurrentThread();
}

// 从 tight 线程回调 Java：取 listener 快照，调用后按需 detach。
void callBytes(jmethodID mid, const tight::Bytes& payload) {
    jobject listener;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        listener = g_listener;
    }
    if (listener == nullptr || mid == nullptr) return;
    bool attached;
    JNIEnv* env = getEnv(&attached);
    if (env == nullptr) return;
    jbyteArray arr = env->NewByteArray(static_cast<jsize>(payload.size()));
    if (arr != nullptr) {
        env->SetByteArrayRegion(arr, 0, static_cast<jsize>(payload.size()),
                                reinterpret_cast<const jbyte*>(payload.data()));
        env->CallVoidMethod(listener, mid, arr);
        env->DeleteLocalRef(arr);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    releaseEnv(attached);
}

void callPeerState(const std::string& peerId, bool online) {
    jobject listener;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        listener = g_listener;
    }
    if (listener == nullptr || g_onPeerState == nullptr) return;
    bool attached;
    JNIEnv* env = getEnv(&attached);
    if (env == nullptr) return;
    jstring id = env->NewStringUTF(peerId.c_str());
    env->CallVoidMethod(listener, g_onPeerState, id, online ? JNI_TRUE : JNI_FALSE);
    env->DeleteLocalRef(id);
    if (env->ExceptionCheck()) env->ExceptionClear();
    releaseEnv(attached);
}

void callLong(jmethodID mid, jlong value) {
    jobject listener;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        listener = g_listener;
    }
    if (listener == nullptr || mid == nullptr) return;
    bool attached;
    JNIEnv* env = getEnv(&attached);
    if (env == nullptr) return;
    env->CallVoidMethod(listener, mid, value);
    if (env->ExceptionCheck()) env->ExceptionClear();
    releaseEnv(attached);
}

void callVoid0(jmethodID mid) {
    jobject listener;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        listener = g_listener;
    }
    if (listener == nullptr || mid == nullptr) return;
    bool attached;
    JNIEnv* env = getEnv(&attached);
    if (env == nullptr) return;
    env->CallVoidMethod(listener, mid);
    if (env->ExceptionCheck()) env->ExceptionClear();
    releaseEnv(attached);
}

// 从 peers() 动态挑选当前发送目标：优先 Online，其次 Established。
// 不能直接缓存 peer id——client 固定端口重连时 server 会复用旧会话并把
// 会话 id 改名成新 id，缓存的旧 id 会查无此人、消息被静默丢弃。
// 注意：调用时不得持有 g_mutex（内部会取 tight 的 peers 锁，锁序见下）。
std::string current_peer_id(tight::TightTransport* transport) {
    if (!transport) return {};
    auto peers = transport->peers();
    std::string fallback;
    for (const auto& p : peers) {
        if (p.state == tight::LinkState::Online) return p.id;
        if (p.state == tight::LinkState::Established && fallback.empty()) fallback = p.id;
    }
    return fallback;
}

}  // namespace

jint JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    return JNI_VERSION_1_6;
}

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_tightcast_server_TightBridge_nativeStart(JNIEnv* env, jclass, jint port, jstring token) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_transport) {
        LOGE("nativeStart: already started");
        return JNI_FALSE;
    }

    const char* tokenStr = env->GetStringUTFChars(token, nullptr);

    tight::TightConfig config;
    config.bind = tight::NetAddress{"0.0.0.0", static_cast<std::uint16_t>(port)};
    config.id = "phone";
    config.token = tokenStr;
    config.role = tight::LinkRole::Node;
    // 协议第 1 节公共项
    config.mtu = 1350;
    config.encryption_enabled = true;
    config.report_interval = std::chrono::milliseconds(333);
    config.late_buffer_ms = 16;
    config.max_message_bytes = 1024 * 1024;
    // 手机内核会把 server 的 UDP socket connect 到首个对端（实测：之后来自
    // 其他地址:端口的报文被 NoPorts 丢弃）。对端掉线后必须重建 transport
    // （新 socket）才能接受新客户端；dead_timeout 收紧到 8s 加快这个过程。
    config.dead_timeout = std::chrono::milliseconds(8000);
    config.retransmit_enabled = true;    // ARQ 机制总开关（channel_reliable 决定哪些通道用）
    // 视频通道（0）不开 ARQ：重传会让丢分片的帧晚于后续帧重组完成、乱序送达
    // 解码器→花屏。视频纯 FEC 兜底 + 缺帧即 REQ_KEYFRAME（应用层恢复）。
    config.channel_reliable[3] = true;   // data 通道可靠
    // 通道级 FEC：ch0 基础层保持开启（默认 true），ch4 增强层关闭——
    // 残差帧丢包即弃（不上屏），校验片带宽让给基础层（protocol §3.4）
    config.channel_fec_enabled[4] = false;
    // USB/RNDIS 高带宽链路：种子/上限提到 100Mbps。贷款保持默认 5s——
    // 实测突发本身不丢包（udpblast 2000 包零丢失，早先的突发丢包其实是
    // tight 每包 printf 拖慢接收线程所致，已宏化关闭）；loan=0 的严格匀速
    // 反而产生排队延迟被 AIMD 误判拥塞 → btl 螺旋触底。
    config.initial_bandwidth_bytes = 12500000;  // 100Mbps
    config.channel_fec_extra[1] = 1;     // 音频通道额外冗余
    config.flush_interval = std::chrono::milliseconds(10);
    env->ReleaseStringUTFChars(token, tokenStr);

    auto transport = std::make_unique<tight::TightTransport>(std::move(config));

    // 通道 1 音频 PCM（client → phone）
    transport->set_message_callback([](const std::string&, tight::Bytes payload) {
        callBytes(g_onAudio, payload);
    });
    // command 通道：触控/键盘/文本/滚动/REQ_KEYFRAME
    transport->set_command_callback([](const std::string&, tight::Bytes payload) {
        callBytes(g_onCommand, payload);
    });
    transport->set_peer_callback([](const tight::PeerEvent& event) {
        if (event.state == tight::LinkState::Online) {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                // 最新 Online 的 peer 优先：旧 client 异常退出后会话要等
                // dead_timeout 才关闭，期间若仍以旧 id 为目标会把视频发丢
                g_peer = event.id;
                g_hasPeer = true;
            }
            LOGI("peer online: %s", event.id.c_str());
            callPeerState(event.id, true);
        } else if (event.state == tight::LinkState::Closed) {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_hasPeer && g_peer == event.id) g_hasPeer = false;
            }
            LOGI("peer closed: %s", event.id.c_str());
            callPeerState(event.id, false);
        }
    });
    // 视频可用码率 → 调整编码码率
    transport->set_video_capacity_callback([](std::uint64_t bps) {
        callLong(g_onVideoCapacity, static_cast<jlong>(bps));
    });
    // 拥塞排空窗口 → 强制新 IDR
    transport->set_evac_keyframe_callback([]() {
        callVoid0(g_onRequestKeyframe);
    });
    // 令牌贷款耗尽 → 强制新 IDR（协议：仅 exhausted=true 映射）
    transport->set_loan_exhausted_callback([](bool exhausted) {
        if (exhausted) callVoid0(g_onRequestKeyframe);
    });

    if (!transport->start()) {
        LOGE("nativeStart: transport start failed (port=%d)", static_cast<int>(port));
        return JNI_FALSE;
    }
    g_transport = std::move(transport);
    LOGI("tight started: bind 0.0.0.0:%d id=phone", static_cast<int>(port));
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_tightcast_server_TightBridge_nativeStop(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_transport) {
        g_transport->stop();
        g_transport.reset();
        g_hasPeer = false;
        g_peer.clear();
        LOGI("tight stopped");
    }
}

JNIEXPORT jboolean JNICALL
Java_com_tightcast_server_TightBridge_nativeSendVideo(JNIEnv* env, jclass,
                                                      jbyteArray frame, jboolean keyframe) {
    // 锁序铁律：先在 g_mutex 下取 transport 快照后立即释放，再调 tight 方法
    // （tight 内部锁）。反过来（持 g_mutex 取 tight 锁）会与 tight 线程持内部
    // 锁触发 Java 回调→nativeSendData 取 g_mutex 形成 ABBA 死锁。
    tight::TightTransport* transport;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        transport = g_transport.get();
        if (transport == nullptr) return JNI_FALSE;
    }
    std::string peer = current_peer_id(transport);
    if (peer.empty()) return JNI_FALSE;
    jsize len = env->GetArrayLength(frame);
    tight::Bytes payload(static_cast<std::size_t>(len));
    env->GetByteArrayRegion(frame, 0, len, reinterpret_cast<jbyte*>(payload.data()));
    bool ok = transport->send_video(peer, std::move(payload), keyframe == JNI_TRUE);
    (ok ? g_videoSendOk : g_videoSendFail).fetch_add(1);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_tightcast_server_TightBridge_nativeSendChannel(JNIEnv* env, jclass,
                                                        jbyteArray payloadArr, jint channel) {
    // 锁序同 nativeSendVideo：g_mutex 只用于取快照（不持锁进 tight 内部锁）
    tight::TightTransport* transport;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        transport = g_transport.get();
        if (transport == nullptr) return JNI_FALSE;
    }
    std::string peer = current_peer_id(transport);
    if (peer.empty()) return JNI_FALSE;
    jsize len = env->GetArrayLength(payloadArr);
    tight::Bytes payload(static_cast<std::size_t>(len));
    env->GetByteArrayRegion(payloadArr, 0, len, reinterpret_cast<jbyte*>(payload.data()));
    return transport->send_channel(peer, std::move(payload),
                                   static_cast<std::uint8_t>(channel)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_tightcast_server_TightBridge_nativeSendData(JNIEnv* env, jclass, jbyteArray payloadArr) {
    // 锁序同 nativeSendVideo：g_mutex 只用于取快照
    tight::TightTransport* transport;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        transport = g_transport.get();
        if (transport == nullptr) return JNI_FALSE;
    }
    std::string peer = current_peer_id(transport);
    if (peer.empty()) return JNI_FALSE;
    jsize len = env->GetArrayLength(payloadArr);
    tight::Bytes payload(static_cast<std::size_t>(len));
    env->GetByteArrayRegion(payloadArr, 0, len, reinterpret_cast<jbyte*>(payload.data()));
    return transport->send_data(peer, std::move(payload)) ? JNI_TRUE : JNI_FALSE;
}

// 诊断：返回 [estimated_bandwidth_bps, btl_bw_bps, video_capacity_bps,
//              outbound_queue_size, video_send_ok, video_send_fail]
JNIEXPORT jlongArray JNICALL
Java_com_tightcast_server_TightBridge_nativeStats(JNIEnv* env, jclass) {
    jlong vals[6] = {0, 0, 0, 0, 0, 0};
    tight::TightTransport* transport;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        transport = g_transport.get();
    }
    if (transport) {   // 快照后调用，勿持 g_mutex 进 tight 锁（ABBA 死锁）
        vals[0] = static_cast<jlong>(transport->estimated_bandwidth_bps());
        vals[1] = static_cast<jlong>(transport->btl_bw_bps());
        vals[2] = static_cast<jlong>(transport->video_capacity_bps());
        vals[3] = static_cast<jlong>(transport->outbound_queue_size());
    }
    vals[4] = static_cast<jlong>(g_videoSendOk.load());
    vals[5] = static_cast<jlong>(g_videoSendFail.load());
    jlongArray arr = env->NewLongArray(6);
    if (arr) env->SetLongArrayRegion(arr, 0, 6, vals);
    return arr;
}

// 诊断：当前发送目标与全部 peer 状态（"chosen=X peers=[id:state ...]"），
// 用于定位僵尸会话抢占发送目标的问题
JNIEXPORT jstring JNICALL
Java_com_tightcast_server_TightBridge_nativePeers(JNIEnv* env, jclass) {
    tight::TightTransport* transport;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        transport = g_transport.get();
    }
    if (!transport) return nullptr;
    std::string s = "chosen=" + current_peer_id(transport) + " peers=[";
    for (const auto& p : transport->peers()) {
        s += p.id + ":" + std::to_string(static_cast<int>(p.state)) + " ";
    }
    s += "]";
    return env->NewStringUTF(s.c_str());
}

JNIEXPORT void JNICALL
Java_com_tightcast_server_TightBridge_nativeSetListener(JNIEnv* env, jclass, jobject listener) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_listener != nullptr) {
        env->DeleteGlobalRef(g_listener);
        g_listener = nullptr;
    }
    if (listener == nullptr) return;
    jclass cls = env->GetObjectClass(listener);
    g_onAudio = env->GetMethodID(cls, "onAudio", "([B)V");
    g_onCommand = env->GetMethodID(cls, "onCommand", "([B)V");
    g_onPeerState = env->GetMethodID(cls, "onPeerState", "(Ljava/lang/String;Z)V");
    g_onVideoCapacity = env->GetMethodID(cls, "onVideoCapacity", "(J)V");
    g_onRequestKeyframe = env->GetMethodID(cls, "onRequestKeyframe", "()V");
    if (g_onAudio == nullptr || g_onCommand == nullptr || g_onPeerState == nullptr ||
        g_onVideoCapacity == nullptr || g_onRequestKeyframe == nullptr) {
        LOGE("nativeSetListener: method id lookup failed");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    g_listener = env->NewGlobalRef(listener);
}

}  // extern "C"
