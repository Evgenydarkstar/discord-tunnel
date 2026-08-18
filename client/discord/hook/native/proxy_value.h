#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace discord_tunnel {

enum class ProxyKind { Http, Socks5 };

struct ProxyValue {
    bool is_specified = false;
    ProxyKind kind = ProxyKind::Http;
    std::string login;
    std::string password;
    std::string host;
    uint16_t port = 0;

    static ProxyValue parse(std::string_view url);

    bool has_auth() const { return !login.empty() && !password.empty(); }
    bool is_http() const  { return is_specified && kind == ProxyKind::Http; }
    bool is_socks5() const { return is_specified && kind == ProxyKind::Socks5; }

    std::string format_http_env() const;     // for http_proxy / https_proxy
    std::string format_chrome_proxy() const; // for --proxy-server=
};

} // namespace discord_tunnel
