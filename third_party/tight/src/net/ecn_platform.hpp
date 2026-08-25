#pragma once

// ECN / L4S (RFC 9331) 仿真平台层。
//
// 背景：Windows (MinGW-w64) 无法读取入站 IP TOS/ECN（WSARecvMsg 未由
// ws2_32.dll 导出），WSASendMsg 的逐包 IP_TOS 控制也被内核拒绝
// (WSAEINVAL)。唯一可靠的原语是 setsockopt(IP_ECN, N) 设置**整个 socket**
// 的出站 ECN codepoint（0=Not-ECT, 1=ECT(1)=L4S, 2=ECT(0), 3=CE）。
//
// 因此 L4S 的"网络标记 CE"在仿真里用一个 distinct-magic 的小 UDP 数据报
// （CE 标记）由 proxy 直接发给接收方承载：接收方数 CE 标记即等价于读到
// CE。发送端仍用 IP_ECN 把出站报文真实标成 ECT(1)（wire 保真，平台支持时
// 可被读取）。

#include <cstdint>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace tight {
namespace tight_detail {
namespace ecn {

enum : uint8_t { kNotEct = 0, kEct1 = 1, kEct0 = 2, kCe = 3 };

// CE 标记数据报 magic：字节 'L4CE'（与 tight 的 kMagic 0x54474854 区分）。
// 载荷仅此 4 字节；接收方据此识别并计数 CE。
inline constexpr std::uint32_t kCeMarkMagic = 0x45434334;  // 'L' '4' 'C' 'E' little-endian

inline bool is_ce_mark(const std::uint8_t* buf, int len) {
    if (len < 4) return false;
    std::uint32_t m;
    std::memcpy(&m, buf, 4);
    return m == kCeMarkMagic;
}

#ifdef _WIN32
#ifndef IP_ECN
#define IP_ECN 50
#endif
#ifndef IP_TOS
#define IP_TOS 3
#endif

// 设置整个 socket 的出站 ECN codepoint（0..3）。Windows 用 IP_ECN。
inline void set_ecn(SOCKET s, uint8_t codepoint) {
    int v = static_cast<int>(codepoint);
    if (setsockopt(s, IPPROTO_IP, IP_ECN, reinterpret_cast<const char*>(&v), sizeof(v)) == 0) return;
    int tos = static_cast<int>(codepoint);
    setsockopt(s, IPPROTO_IP, IP_TOS, reinterpret_cast<const char*>(&tos), sizeof(tos));
}

#else

inline void set_ecn(int s, uint8_t codepoint) {
    int tos = static_cast<int>(codepoint);
    setsockopt(s, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
}

#endif

// 发送端把出站报文标成 ECT(1)（L4S 标识）。
template <typename Sock>
inline void set_ect1(Sock s) {
    set_ecn(s, kEct1);
}

}  // namespace ecn
}  // namespace tight_detail
}  // namespace tight
