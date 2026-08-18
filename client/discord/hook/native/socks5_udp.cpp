// SOCKS5 UDP ASSOCIATE - see ../discord-tunnel/ISSUE-65-IDEAS.md section 3.2.
//
// Protocol summary:
//   1. Open a TCP socket to the SOCKS5 server. Keep it alive - closing it
//      ends the UDP association per RFC 1928 section 7.
//   2. Method-selection handshake. If proxy_value has auth, offer
//      USERNAME/PASSWORD per RFC 1929; otherwise NO AUTH.
//   3. Send UDP ASSOCIATE request (CMD=0x03). Server reply contains the
//      relay endpoint (BND.ADDR / BND.PORT) we should send our UDP
//      packets to.
//   4. For each outgoing Discord UDP packet, prepend a SOCKS5 UDP
//      request header and send to the relay instead of the real
//      destination. The relay forwards to the real destination.
//   5. Inbound packets from the relay arrive with the same SOCKS5
//      header wrapping (RSV + FRAG + ATYP + SRC.ADDR + SRC.PORT + DATA).
//      We strip it in unwrap_reply().

#include "socks5_udp.h"

#include <atomic>
#include <cstring>
#include <vector>

#if !DT_OS_WIN
#include <netdb.h>
#endif

#include "config.h"
#include "logging.h"
#include "real_io.h"

namespace discord_tunnel {

Socks5UdpRelay g_socks5_udp;

namespace {

void close_socket(dt_socket_t s) {
#if DT_OS_WIN
    closesocket(s);
#else
    close(s);
#endif
}

bool send_all(dt_socket_t s, const void* buf, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(buf);
    std::size_t total = 0;
    while (total < n) {
#if DT_OS_WIN
        int r = ::send(s, reinterpret_cast<const char*>(p + total),
                       static_cast<int>(n - total), 0);
#else
        ssize_t r = ::send(s, p + total, n - total, 0);
#endif
        if (r <= 0) return false;
        total += static_cast<std::size_t>(r);
    }
    return true;
}

bool recv_exact(dt_socket_t s, void* buf, std::size_t n) {
    auto* p = static_cast<std::uint8_t*>(buf);
    std::size_t total = 0;
    while (total < n) {
#if DT_OS_WIN
        int r = ::recv(s, reinterpret_cast<char*>(p + total),
                       static_cast<int>(n - total), 0);
#else
        ssize_t r = ::recv(s, p + total, n - total, 0);
#endif
        if (r <= 0) return false;
        total += static_cast<std::size_t>(r);
    }
    return true;
}

bool tcp_connect_ipv4(dt_socket_t s, const std::string& host, std::uint16_t port) {
    sockaddr_in sa = {};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);

    in_addr ip = {};
    if (inet_pton(AF_INET, host.c_str(), &ip) != 1) {
        // host wasn't a dotted IPv4 literal - try DNS.
        addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
            return false;
        }
        ip = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    sa.sin_addr = ip;
    return ::connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0;
}

// RFC 1928 section 3 - method selection. RFC 1929 - username/password subnegotiation.
bool socks5_authenticate(dt_socket_t tcp, const ProxyValue& proxy) {
    const bool use_auth = proxy.has_auth();
    std::uint8_t hello[4];
    int hello_len;
    if (use_auth) {
        hello[0] = 0x05; hello[1] = 0x02; hello[2] = 0x00; hello[3] = 0x02;
        hello_len = 4;
    } else {
        hello[0] = 0x05; hello[1] = 0x01; hello[2] = 0x00;
        hello_len = 3;
    }
    if (!send_all(tcp, hello, hello_len)) {
        LOG_WARN("[socks5-udp] failed to send auth hello");
        return false;
    }

    std::uint8_t reply[2];
    if (!recv_exact(tcp, reply, 2)) {
        LOG_WARN("[socks5-udp] failed to read auth reply header");
        return false;
    }
    if (reply[0] != 0x05) {
        LOG_WARN("[socks5-udp] invalid auth reply version: 0x{:02x}", reply[0]);
        return false;
    }

    switch (reply[1]) {
    case 0x00:   // NO AUTH selected
        return true;
    case 0x02: { // USERNAME/PASSWORD selected
        if (!use_auth) return false; // server demands what we don't have
        std::vector<std::uint8_t> req;
        req.reserve(3 + proxy.login.size() + proxy.password.size());
        req.push_back(0x01);
        req.push_back(static_cast<std::uint8_t>(proxy.login.size()));
        req.insert(req.end(), proxy.login.begin(), proxy.login.end());
        req.push_back(static_cast<std::uint8_t>(proxy.password.size()));
        req.insert(req.end(), proxy.password.begin(), proxy.password.end());
        if (!send_all(tcp, req.data(), req.size())) {
            LOG_WARN("[socks5-udp] failed to send username/password auth request");
            return false;
        }
        std::uint8_t auth_reply[2];
        if (!recv_exact(tcp, auth_reply, 2)) {
            LOG_WARN("[socks5-udp] failed to read username/password auth reply");
            return false;
        }
        if (!(auth_reply[0] == 0x01 && auth_reply[1] == 0x00)) {
            LOG_WARN("[socks5-udp] username/password auth rejected: VER=0x{:02x} STATUS=0x{:02x}",
                     auth_reply[0], auth_reply[1]);
        }
        return auth_reply[0] == 0x01 && auth_reply[1] == 0x00;
    }
    default:
        LOG_WARN("[socks5-udp] proxy selected unsupported auth method: 0x{:02x}", reply[1]);
        return false;
    }
}

// RFC 1928 section 4 - UDP ASSOCIATE request, parse BND.ADDR / BND.PORT.
bool socks5_udp_associate(dt_socket_t tcp, sockaddr_in* out_relay) {
    // VER, CMD=UDP_ASSOCIATE(3), RSV, ATYP=IPv4(1), DST.ADDR=0.0.0.0, DST.PORT=0
    const std::uint8_t req[] = {0x05, 0x03, 0x00, 0x01, 0,0,0,0, 0,0};
    if (!send_all(tcp, req, sizeof(req))) {
        LOG_WARN("[socks5-udp] failed to send UDP ASSOCIATE request");
        return false;
    }

    std::uint8_t header[4];
    if (!recv_exact(tcp, header, 4)) {
        LOG_WARN("[socks5-udp] failed to read UDP ASSOCIATE reply header");
        return false;
    }
    if (header[0] != 0x05) {
        LOG_WARN("[socks5-udp] invalid UDP ASSOCIATE reply version: 0x{:02x}", header[0]);
        return false;
    }
    if (header[1] != 0x00) {
        LOG_WARN("[socks5-udp] server rejected UDP ASSOCIATE: REP=0x{:02x}", header[1]);
        return false;
    }

    out_relay->sin_family = AF_INET;
    switch (header[3]) {
    case 0x01: { // IPv4
        std::uint8_t ip[4];
        if (!recv_exact(tcp, ip, 4)) {
            LOG_WARN("[socks5-udp] failed to read IPv4 relay address");
            return false;
        }
        std::memcpy(&out_relay->sin_addr, ip, 4);
        break;
    }
    case 0x03: { // Domain
        std::uint8_t dlen;
        if (!recv_exact(tcp, &dlen, 1)) {
            LOG_WARN("[socks5-udp] failed to read domain relay length");
            return false;
        }
        char domain[256];
        if (!recv_exact(tcp, domain, dlen)) {
            LOG_WARN("[socks5-udp] failed to read domain relay value");
            return false;
        }
        domain[dlen] = 0;
        if (inet_pton(AF_INET, domain, &out_relay->sin_addr) != 1) {
            LOG_WARN("[socks5-udp] relay domain is not an IPv4 literal: {}", domain);
            return false;
        }
        break;
    }
    case 0x04: // IPv6
        LOG_WARN("[socks5-udp] server returned IPv6 relay, not yet supported");
        return false;
    default:
        LOG_WARN("[socks5-udp] unsupported UDP ASSOCIATE ATYP: 0x{:02x}", header[3]);
        return false;
    }
    std::uint8_t port[2];
    if (!recv_exact(tcp, port, 2)) {
        LOG_WARN("[socks5-udp] failed to read relay port");
        return false;
    }
    std::memcpy(&out_relay->sin_port, port, 2);
    return true;
}

} // anonymous namespace

bool Socks5UdpRelay::enabled() const {
    return g_options.socks5_udp_associate && g_options.proxy.is_socks5();
}

bool Socks5UdpRelay::has_association(dt_socket_t udp_sock) const {
    std::lock_guard<std::mutex> g(mutex_);
    auto it = by_socket_.find(udp_sock);
    return it != by_socket_.end() && it->second.ready;
}

bool Socks5UdpRelay::ensure_association(dt_socket_t udp_sock) {
    if (!enabled()) return false;
    {
        std::lock_guard<std::mutex> g(mutex_);
        auto it = by_socket_.find(udp_sock);
        if (it != by_socket_.end() && it->second.ready) return true;
    }

    dt_socket_t tcp = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (tcp == DT_INVALID_SOCKET_V) {
        LOG_WARN("[socks5-udp] failed to create TCP control socket");
        return false;
    }

    const auto& proxy = g_options.proxy;
    if (!tcp_connect_ipv4(tcp, proxy.host, proxy.port)) {
        LOG_WARN("[socks5-udp] connect to {}:{} failed", proxy.host, proxy.port);
        close_socket(tcp);
        return false;
    }
    if (!socks5_authenticate(tcp, proxy)) {
        LOG_WARN("[socks5-udp] authentication failed");
        close_socket(tcp);
        return false;
    }
    sockaddr_in relay = {};
    if (!socks5_udp_associate(tcp, &relay)) {
        LOG_WARN("[socks5-udp] UDP ASSOCIATE request failed");
        close_socket(tcp);
        return false;
    }

    // If the server returned 0.0.0.0 in BND.ADDR (a common shortcut meaning
    // "send to the same IP you opened the TCP control channel to"), use
    // the proxy host's IPv4 instead.
    if (relay.sin_addr.s_addr == 0) {
        if (inet_pton(AF_INET, proxy.host.c_str(), &relay.sin_addr) != 1) {
            // proxy.host wasn't a dotted IPv4 - DNS-resolve it.
            addrinfo hints = {};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* res = nullptr;
            if (getaddrinfo(proxy.host.c_str(), nullptr, &hints, &res) == 0 && res) {
                relay.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
                freeaddrinfo(res);
            }
        }
    }

    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &relay.sin_addr, ip, INET_ADDRSTRLEN);
    LOG_INFO("[socks5-udp] association ready for sock={}: relay {}:{}",
             static_cast<unsigned long long>(udp_sock), ip, ntohs(relay.sin_port));

    std::lock_guard<std::mutex> g(mutex_);
    by_socket_[udp_sock] = {tcp, relay, true};
    return true;
}

dt_ssize_t Socks5UdpRelay::send_via_relay(dt_socket_t udp_sock,
                                          const void* buf, std::size_t len,
                                          const sockaddr* dest, dt_socklen_t /*dest_len*/) {
    std::vector<std::uint8_t> pkt;
    sockaddr_in relay = {};
    if (!prepare_for_relay(udp_sock, buf, len, dest, pkt, relay)) return -1;

    auto sent = real_io::send_udp(udp_sock, pkt.data(), pkt.size(),
                                  reinterpret_cast<const sockaddr*>(&relay),
                                  sizeof(relay));
    if (sent < 0) {
#if DT_OS_WIN
        static std::atomic<unsigned> failures{0};
        if (failures.fetch_add(1, std::memory_order_relaxed) < 8) {
            LOG_WARN("[socks5-udp] relay sendto failed for sock={}: WSA error {}",
                     static_cast<unsigned long long>(udp_sock), WSAGetLastError());
        }
#endif
        return -1;
    }
    // Callers compare the return against the size they passed in, not against
    // the wrapped size. Lie about the wrapper bytes.
    return static_cast<dt_ssize_t>(len);
}

bool Socks5UdpRelay::prepare_for_relay(dt_socket_t udp_sock,
                                       const void* buf, std::size_t len,
                                       const sockaddr* dest,
                                       std::vector<std::uint8_t>& pkt,
                                       sockaddr_in& relay_addr) {
    if (!dest || dest->sa_family != AF_INET) return false;
    const auto* d = reinterpret_cast<const sockaddr_in*>(dest);

    UdpAssociation assoc;
    {
        std::lock_guard<std::mutex> g(mutex_);
        auto it = by_socket_.find(udp_sock);
        if (it == by_socket_.end() || !it->second.ready) return false;
        assoc = it->second;
    }

    //  +----+------+------+----+----------+----+
    //  |RSV | FRAG | ATYP | IP |   PORT   |DATA|
    //  +----+------+------+----+----------+----+
    //  | 2  |  1   |  1   | 4  |    2     | N  |
    pkt.clear();
    pkt.reserve(10 + len);
    pkt.push_back(0); pkt.push_back(0);   // RSV
    pkt.push_back(0);                      // FRAG
    pkt.push_back(0x01);                   // ATYP IPv4
    const auto* ip   = reinterpret_cast<const std::uint8_t*>(&d->sin_addr);
    pkt.insert(pkt.end(), ip, ip + 4);
    const auto* port = reinterpret_cast<const std::uint8_t*>(&d->sin_port);
    pkt.insert(pkt.end(), port, port + 2);
    const auto* data = static_cast<const std::uint8_t*>(buf);
    pkt.insert(pkt.end(), data, data + len);

    relay_addr = assoc.relay_addr;
    return true;
}

dt_ssize_t Socks5UdpRelay::unwrap_reply(std::uint8_t* buf, std::size_t len,
                                        sockaddr* out_src, dt_socklen_t* out_src_len) {
    if (len < 10) return -1;
    if (buf[0] != 0 || buf[1] != 0) return -1;     // RSV must be zero
    if (buf[2] != 0) return -1;                     // FRAG: we don't reassemble

    std::size_t hdr_len = 0;
    sockaddr_in src = {};
    src.sin_family = AF_INET;

    switch (buf[3]) {
    case 0x01: // IPv4
        hdr_len = 4 + 4 + 2;
        if (len < hdr_len) return -1;
        std::memcpy(&src.sin_addr, buf + 4, 4);
        std::memcpy(&src.sin_port, buf + 8, 2);
        break;
    case 0x03: { // Domain
        std::uint8_t dlen = buf[4];
        hdr_len = 4 + 1 + dlen + 2;
        if (len < hdr_len) return -1;
        char domain[256] = {0};
        std::memcpy(domain, buf + 5, dlen);
        if (inet_pton(AF_INET, domain, &src.sin_addr) != 1) return -1;
        std::memcpy(&src.sin_port, buf + 5 + dlen, 2);
        break;
    }
    case 0x04:
        return -1; // IPv6 not supported
    default:
        return -1;
    }

    std::size_t payload_len = len - hdr_len;
    std::memmove(buf, buf + hdr_len, payload_len);

    if (out_src && out_src_len) {
        auto want = static_cast<dt_socklen_t>(sizeof(sockaddr_in));
        auto copy = (*out_src_len < want) ? *out_src_len : want;
        std::memcpy(out_src, &src, copy);
        *out_src_len = want;
    }
    return static_cast<dt_ssize_t>(payload_len);
}

void Socks5UdpRelay::release(dt_socket_t udp_sock) {
    dt_socket_t control_tcp = DT_INVALID_SOCKET_V;
    {
        std::lock_guard<std::mutex> g(mutex_);
        auto it = by_socket_.find(udp_sock);
        if (it == by_socket_.end()) return;
        control_tcp = it->second.control_tcp;
        by_socket_.erase(it);
    }
    // closesocket is hooked on Windows and re-enters release(). Never call it
    // while holding mutex_, otherwise the recursive lookup deadlocks.
    if (control_tcp != DT_INVALID_SOCKET_V) {
        close_socket(control_tcp);
    }
}

} // namespace discord_tunnel
