#include "net/wsa.hpp"

#ifdef _WIN32
#include "net/socket_platform.hpp"

#include <mutex>
#endif

namespace tight::tight_detail {

#ifdef _WIN32

namespace {

struct WsaRef {
    std::mutex m_mu;
    int m_refcount{0};
    bool m_ok{false};
};

WsaRef& wsa_ref() {
    static WsaRef r;
    return r;
}

}

bool wsa_acquire() {
    auto& r = wsa_ref();
    std::lock_guard<std::mutex> lock(r.m_mu);
    if (!r.m_ok) {
        WSADATA wsa;
        r.m_ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    }
    if (r.m_ok) ++r.m_refcount;
    return r.m_ok;
}

void wsa_release() {
    auto& r = wsa_ref();
    std::lock_guard<std::mutex> lock(r.m_mu);
    if (r.m_refcount > 0) {
        --r.m_refcount;
        if (r.m_refcount == 0 && r.m_ok) {
            WSACleanup();
            r.m_ok = false;
        }
    }
}

#else

bool wsa_acquire() {
    return true;
}

void wsa_release() {
}

#endif

}
