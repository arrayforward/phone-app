#pragma once

// Internal Winsock2 reference-counting helper (Windows only; no-op on POSIX).
// wsa_acquire/wsa_release keep WSAStartup/WSACleanup balanced across multiple
// TightTransport instances. Not part of the public API.

namespace tight::tight_detail {

bool wsa_acquire();
void wsa_release();

}
