#include "net/address.hpp"

#include <cstring>

namespace tight::tight_detail {

bool resolve_address(const std::string& host, std::uint16_t port, sockaddr_in& out) {
    std::memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    if (host.empty() || host == "0.0.0.0") {
        out.sin_addr.s_addr = htonl(INADDR_ANY);
        return true;
    }
    if (host == "127.0.0.1" || host == "localhost") {
        out.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return true;
    }
    in_addr addr;
    if (inet_pton(AF_INET, host.c_str(), &addr) == 1) {
        out.sin_addr = addr;
        return true;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        return false;
    }
    bool ok = false;
    for (addrinfo* p = res; p; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            auto* sin = reinterpret_cast<sockaddr_in*>(p->ai_addr);
            out.sin_addr = sin->sin_addr;
            ok = true;
            break;
        }
    }
    freeaddrinfo(res);
    return ok;
}

std::string address_host(const sockaddr_in& addr) {
    char buf[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf)) == nullptr) {
        return {};
    }
    return buf;
}

}
