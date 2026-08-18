#include "proxy_value.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>

namespace discord_tunnel {

namespace {
    std::string to_lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }
    std::string trim(std::string s) {
        auto ns = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), ns));
        s.erase(std::find_if(s.rbegin(), s.rend(), ns).base(), s.end());
        return s;
    }
}

ProxyValue ProxyValue::parse(std::string_view url) {
    static const std::regex re(
        R"(^(?:([a-zA-Z\d]+)://)?(?:(.+):(.+)@)?(.+):(\d+)$)",
        std::regex::ECMAScript);

    ProxyValue v;
    std::string s = trim(std::string{url});
    if (s.empty()) return v;

    std::smatch m;
    if (!std::regex_match(s, m, re)) return v;

    v.is_specified = true;
    std::string proto = to_lower(m[1].str());
    if (proto == "socks5") v.kind = ProxyKind::Socks5;
    else                   v.kind = ProxyKind::Http; // http, https -> http
    v.login    = trim(m[2].str());
    v.password = trim(m[3].str());
    v.host     = trim(m[4].str());
    v.port     = static_cast<uint16_t>(std::strtoul(m[5].str().c_str(), nullptr, 10));
    return v;
}

std::string ProxyValue::format_http_env() const {
    if (!is_specified) return {};
    std::string out = "http://";
    if (has_auth()) out += login + ":" + password + "@";
    out += host + ":" + std::to_string(port);
    return out;
}

std::string ProxyValue::format_chrome_proxy() const {
    if (!is_specified) return {};
    std::string scheme = (kind == ProxyKind::Socks5) ? "socks5" : "http";
    return scheme + "://" + host + ":" + std::to_string(port);
}

} // namespace discord_tunnel
