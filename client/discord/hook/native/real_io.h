// Portable handle to the *unhooked* socket I/O functions.
//
// The per-OS shim (windows/hooks.cpp on Windows, posix/preload.cpp on
// Linux/macOS) is responsible for setting up the real function pointers
// during init. Core code (UDP strategies, SOCKS5 relay) calls into this
// module rather than the raw OS function - that way the strategy code
// stays platform-agnostic and never recurses through our own hooks.

#pragma once

#include "platform.h"

namespace discord_tunnel::real_io {

// Send a UDP datagram via the real sendto (bypassing any installed hook).
// Returns the number of bytes sent or a negative value on error.
dt_ssize_t send_udp(dt_socket_t sock,
                    const void* buf, std::size_t len,
                    const sockaddr* to, dt_socklen_t to_len);

// Set by the per-OS shim during initialization. Each platform stores a
// function pointer that calls the unhooked OS version of sendto. If null,
// `send_udp` falls back to invoking the system function directly (safe
// only if there's no hook in place yet, i.e. before the hook is enabled).
#if DT_OS_WIN
using winsock_sendto_t = int (WSAAPI*)(SOCKET, const char*, int, int,
                                       const sockaddr*, int);
extern winsock_sendto_t real_sendto_win;
#else
using posix_sendto_t = ssize_t (*)(int, const void*, std::size_t, int,
                                   const sockaddr*, socklen_t);
extern posix_sendto_t real_sendto_posix;
#endif

} // namespace discord_tunnel::real_io
