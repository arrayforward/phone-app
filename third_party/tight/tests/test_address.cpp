#include "test_framework.hpp"

#include "net/address.hpp"

#include <cstring>

using namespace tight::tight_detail;

TEST_CASE(address_any_host) {
    sockaddr_in out{};
    CHECK(resolve_address("", 9443, out));
    CHECK_EQ(out.sin_port, htons(9443));
    CHECK_EQ(out.sin_addr.s_addr, htonl(INADDR_ANY));

    CHECK(resolve_address("0.0.0.0", 1, out));
    CHECK_EQ(out.sin_addr.s_addr, htonl(INADDR_ANY));
}

TEST_CASE(address_loopback) {
    sockaddr_in out{};
    CHECK(resolve_address("127.0.0.1", 80, out));
    CHECK_EQ(out.sin_addr.s_addr, htonl(INADDR_LOOPBACK));
    CHECK(resolve_address("localhost", 80, out));
    CHECK_EQ(out.sin_addr.s_addr, htonl(INADDR_LOOPBACK));
}

TEST_CASE(address_numeric_ipv4) {
    sockaddr_in out{};
    CHECK(resolve_address("10.20.30.40", 1234, out));
    CHECK_EQ(out.sin_addr.s_addr, htonl(0x0A141E28U));
}

TEST_CASE(address_invalid_fails) {
    sockaddr_in out{};
    // 超出字节范围，inet_pton 与 getaddrinfo 均应失败
    CHECK(!resolve_address("999.999.999.999", 80, out));
    // RFC 2606 保留 .invalid TLD，永不解析
    CHECK(!resolve_address("definitely-not-a-real-host.invalid", 80, out));
}

TEST_CASE(address_port_preserved) {
    sockaddr_in out{};
    CHECK(resolve_address("127.0.0.1", 65535, out));
    CHECK_EQ(out.sin_port, htons(65535));
}
