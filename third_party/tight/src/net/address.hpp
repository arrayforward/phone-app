#pragma once

// Internal IPv4 address resolution helper (numeric hosts, "localhost", and
// DNS names). Not part of the public API.

#include "net/socket_platform.hpp"

#include <cstdint>
#include <string>

namespace tight::tight_detail {

bool resolve_address(const std::string& host, std::uint16_t port, sockaddr_in& out);

// sockaddr_in → 点分十进制 host 字符串（不含端口）。
std::string address_host(const sockaddr_in& addr);

}
