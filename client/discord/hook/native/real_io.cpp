#include "real_io.h"

namespace discord_tunnel::real_io {

#if DT_OS_WIN
winsock_sendto_t real_sendto_win = nullptr;
#else
posix_sendto_t real_sendto_posix = nullptr;
#endif

dt_ssize_t send_udp(dt_socket_t sock,
                    const void* buf, std::size_t len,
                    const sockaddr* to, dt_socklen_t to_len) {
    if (len == 0) return 0;

#if DT_OS_WIN
    if (real_sendto_win) {
        return real_sendto_win(sock,
                               static_cast<const char*>(buf),
                               static_cast<int>(len),
                               0, to, to_len);
    }
    return ::sendto(sock,
                    static_cast<const char*>(buf),
                    static_cast<int>(len),
                    0, to, to_len);
#else
    if (real_sendto_posix) {
        return real_sendto_posix(sock, buf, len, 0, to, to_len);
    }
    return ::sendto(sock, buf, len, 0, to, to_len);
#endif
}

} // namespace discord_tunnel::real_io
