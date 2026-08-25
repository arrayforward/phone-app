#pragma once

// Internal platform socket layer: on Windows we use the Winsock2 API (WSA*),
// on POSIX we use the Berkeley sockets API. These wrappers keep the rest of
// the tight transport platform-agnostic. Not part of the public API.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif

namespace tight::tight_detail {

#ifdef _WIN32
using NativeSocket = SOCKET;
using SockLen = int;
inline constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
// SIO_UDP_CONNRESET (0x9800000C) is defined in newer Windows SDKs but not
// all build environments; define it here for portability.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET 0x9800000CUL
#endif
inline void close_socket(NativeSocket s) { closesocket(s); }
inline int last_socket_error() { return WSAGetLastError(); }
inline bool would_block(int e) { return e == WSAEWOULDBLOCK; }
inline int tight_setsockopt(NativeSocket s, int level, int optname,
                            const char* optval, int optlen) {
    return ::setsockopt(s, level, optname, optval, optlen);
}
inline int tight_recvfrom(NativeSocket s, char* buf, int len, int flags,
                          sockaddr* from, SockLen* fromlen) {
    return ::recvfrom(s, buf, len, flags, from, fromlen);
}
inline int tight_sendto(NativeSocket s, const char* buf, int len, int flags,
                        const sockaddr* to, int tolen) {
    return ::sendto(s, buf, len, flags, to, tolen);
}
#else
using NativeSocket = int;
using SockLen = socklen_t;
inline constexpr NativeSocket kInvalidSocket = -1;
inline void close_socket(NativeSocket s) { ::close(s); }
inline int last_socket_error() { return errno; }
inline bool would_block(int e) { return e == EWOULDBLOCK || e == EAGAIN; }
inline int tight_setsockopt(NativeSocket s, int level, int optname,
                            const char* optval, int optlen) {
    return ::setsockopt(s, level, optname, optval, optlen);
}
inline int tight_recvfrom(NativeSocket s, char* buf, int len, int flags,
                          sockaddr* from, SockLen* fromlen) {
    return ::recvfrom(s, buf, len, flags, from, fromlen);
}
inline int tight_sendto(NativeSocket s, const char* buf, int len, int flags,
                        const sockaddr* to, int tolen) {
    return ::sendto(s, buf, len, flags, to, tolen);
}
#endif

}
