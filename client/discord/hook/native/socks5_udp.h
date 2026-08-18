// SOCKS5 UDP ASSOCIATE - RFC 1928 section 7 + RFC 1929 for username/password auth.
//
// One association per UDP socket. The TCP control channel is kept alive
// for the lifetime of the UDP socket (closing it tears the association
// down per RFC).

#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "platform.h"
#include "proxy_value.h"

namespace discord_tunnel {

struct UdpAssociation {
    dt_socket_t control_tcp = DT_INVALID_SOCKET_V;
    sockaddr_in relay_addr  = {};
    bool ready = false;
};

class Socks5UdpRelay {
public:
    // True when SOCKS5 UDP ASSOCIATE is enabled in discord-tunnel.ini and the
    // configured proxy is socks5.
    bool enabled() const;

    // Returns true if this UDP socket already has a working association.
    // Cheap probe used by recvfrom hooks to decide whether to unwrap.
    bool has_association(dt_socket_t udp_sock) const;

    // Set up the UDP ASSOCIATE control channel for `udp_sock`. Returns
    // false on failure (caller should fall back to local UDP strategies).
    // Idempotent - repeat calls on the same socket are no-ops.
    bool ensure_association(dt_socket_t udp_sock);

    // Wrap an outgoing UDP packet in a SOCKS5 UDP request header and
    // redirect it to the relay endpoint. Returns the original payload
    // length on success (callers expect "how many bytes did you send"
    // to match what they asked for), or a negative value on failure.
    dt_ssize_t send_via_relay(dt_socket_t udp_sock,
                              const void* buf, std::size_t len,
                              const sockaddr* dest, dt_socklen_t dest_len);

    // Build a wrapped packet without sending it. The Windows overlapped
    // hooks use this to preserve IOCP completion semantics.
    bool prepare_for_relay(dt_socket_t udp_sock,
                           const void* buf, std::size_t len,
                           const sockaddr* dest,
                           std::vector<std::uint8_t>& pkt,
                           sockaddr_in& relay_addr);

    // Strip the SOCKS5 UDP reply header from a freshly received packet,
    // shift the payload to the start of `buf` in-place, and write the
    // unwrapped source address into `out_src`. Returns the payload length
    // or a negative value if the header looks invalid.
    dt_ssize_t unwrap_reply(std::uint8_t* buf, std::size_t len,
                            sockaddr* out_src, dt_socklen_t* out_src_len);

    // Tear down the association for a socket (called when Discord closes
    // a UDP socket, or on a fresh socket reuse). Closes the TCP channel.
    void release(dt_socket_t udp_sock);

private:
    mutable std::mutex mutex_;
    std::unordered_map<dt_socket_t, UdpAssociation> by_socket_;
};

extern Socks5UdpRelay g_socks5_udp;

} // namespace discord_tunnel
