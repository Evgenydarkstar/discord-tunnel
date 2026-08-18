// Minimal INI reader. We deliberately do NOT pull in a third-party library
// here - the DLL must stay tiny and dependency-free at the C++ level.

#include "config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace discord_tunnel {

std::filesystem::path g_current_dir;
TunnelOptions g_options;

namespace {

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool parse_bool(std::string_view raw) {
    auto s = to_lower(trim(std::string{raw}));
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

std::vector<std::vector<uint8_t>> parse_prefix_packets(std::string_view raw) {
    std::vector<std::vector<uint8_t>> out;
    std::string cur;
    auto flush = [&]() {
        cur = trim(cur);
        if (cur.rfind("0x", 0) == 0 || cur.rfind("0X", 0) == 0) cur.erase(0, 2);
        if (cur.empty() || cur.size() % 2 != 0) { cur.clear(); return; }
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i < cur.size(); i += 2) {
            char buf[3] = {cur[i], cur[i + 1], 0};
            char* end = nullptr;
            unsigned long v = std::strtoul(buf, &end, 16);
            if (end != buf + 2) { cur.clear(); return; }
            bytes.push_back(static_cast<uint8_t>(v));
        }
        out.push_back(std::move(bytes));
        cur.clear();
    };
    for (char c : raw) {
        if (c == ',' || c == ';' || std::isspace(static_cast<unsigned char>(c))) {
            flush();
        } else {
            cur.push_back(c);
        }
    }
    flush();
    return out;
}

struct IniTable {
    // [section][key] = value
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> sections;

    std::string get(const std::string& section, const std::string& key, std::string def = {}) const {
        auto sec_it = sections.find(section);
        if (sec_it == sections.end()) return def;
        auto key_it = sec_it->second.find(key);
        if (key_it == sec_it->second.end()) return def;
        return key_it->second;
    }

    bool has(const std::string& section, const std::string& key) const {
        auto sec_it = sections.find(section);
        if (sec_it == sections.end()) return false;
        return sec_it->second.count(key) > 0;
    }
};

IniTable load_ini(const std::filesystem::path& path) {
    IniTable table;
    std::ifstream f(path);
    if (!f) return table;

    std::string line, current_section;
    while (std::getline(f, line)) {
        // strip BOM on first line
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) {
            line.erase(0, 3);
        }
        auto t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        if (t.front() == '[' && t.back() == ']') {
            current_section = to_lower(t.substr(1, t.size() - 2));
            continue;
        }
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        auto key = to_lower(trim(t.substr(0, eq)));
        auto val = trim(t.substr(eq + 1));
        table.sections[current_section][key] = val;
    }
    return table;
}

} // namespace

UdpStrategy parse_udp_strategy(std::string_view s) {
    auto lower = to_lower(std::string{s});
    if (lower == "off" || lower == "disable" || lower == "disabled") return UdpStrategy::Off;
    if (lower == "uae_v1" || lower == "uae") return UdpStrategy::UaeV1;
    if (lower == "uae_v2") return UdpStrategy::UaeV2;
    if (lower == "split") return UdpStrategy::Split;
    if (lower == "custom") return UdpStrategy::Custom;
    return UdpStrategy::Classic;
}

std::string_view udp_strategy_name(UdpStrategy s) {
    switch (s) {
    case UdpStrategy::Off:     return "off";
    case UdpStrategy::Classic: return "classic";
    case UdpStrategy::UaeV1:   return "uae_v1";
    case UdpStrategy::UaeV2:   return "uae_v2";
    case UdpStrategy::Split:   return "split";
    case UdpStrategy::Custom:  return "custom";
    }
    return "classic";
}

TunnelOptions Config::load(const std::filesystem::path& ini_path) {
    TunnelOptions opt;
    auto t = load_ini(ini_path);

    auto get = [&](std::string_view section, std::string_view key,
                   std::string_view legacy_section = {},
                   std::string_view legacy_key = {}) {
        if (t.has(std::string(section), std::string(key))) {
            return t.get(std::string(section), std::string(key));
        }
        if (!legacy_key.empty() && t.has(std::string(legacy_section), std::string(legacy_key))) {
            return t.get(std::string(legacy_section), std::string(legacy_key));
        }
        return std::string{};
    };

    auto has = [&](std::string_view section, std::string_view key,
                   std::string_view legacy_section = {},
                   std::string_view legacy_key = {}) {
        return t.has(std::string(section), std::string(key)) ||
            (!legacy_key.empty() && t.has(std::string(legacy_section), std::string(legacy_key)));
    };

    opt.tunnel.enabled = has("tunnel", "enabled")
        ? parse_bool(get("tunnel", "enabled"))
        : true;
    opt.tunnel.server = get("tunnel", "server", "", "server");
    if (has("tunnel", "port", "", "port")) {
        opt.tunnel.port = static_cast<uint16_t>(std::stoul(get("tunnel", "port", "", "port")));
    }
    opt.tunnel.token = get("tunnel", "token", "", "token");
    if (has("tunnel", "ca_cert", "", "ca_cert_path")) {
        auto ca_cert = trim(get("tunnel", "ca_cert", "", "ca_cert_path"));
        if (!ca_cert.empty()) {
            opt.tunnel.ca_cert = std::filesystem::path(ca_cert);
        }
    }
    if (has("tunnel", "insecure", "", "skip_tls_verify")) {
        opt.tunnel.insecure = parse_bool(get("tunnel", "insecure", "", "skip_tls_verify"));
    }
    if (has("tunnel", "listen_port")) {
        opt.tunnel.listen_port = static_cast<uint16_t>(std::stoul(get("tunnel", "listen_port")));
    }

    opt.proxy_url = get("tunnel", "proxy", "", "proxy");
    if (opt.tunnel.enabled) {
        opt.proxy_url = "http://127.0.0.1:" + std::to_string(opt.tunnel.listen_port);
    }
    opt.proxy = ProxyValue::parse(opt.proxy_url);

    if (t.has("udp", "strategy")) {
        opt.udp.strategy = parse_udp_strategy(t.get("udp", "strategy"));
    }
    if (t.has("udp", "prefix_delay_ms")) {
        opt.udp.prefix_delay_ms = static_cast<uint32_t>(std::stoul(t.get("udp", "prefix_delay_ms")));
    }
    if (t.has("udp", "prefix_packets")) {
        opt.udp.prefix_packets = parse_prefix_packets(t.get("udp", "prefix_packets"));
    }
    if (t.has("udp", "split_first")) {
        opt.udp.split_first = static_cast<uint8_t>(std::stoul(t.get("udp", "split_first")));
    }
    if (t.has("udp", "force_tcp_fallback")) {
        opt.udp.force_tcp_fallback = parse_bool(t.get("udp", "force_tcp_fallback"));
    }
    if (t.has("socks5", "udp_associate")) {
        opt.socks5_udp_associate = parse_bool(t.get("socks5", "udp_associate"));
    }
    if (opt.tunnel.enabled) {
        opt.socks5_udp_associate = true;
    }
    if (t.has("logging", "level"))   opt.log_level   = t.get("logging", "level");
    if (t.has("logging", "file_enabled")) opt.log_file_enabled = parse_bool(t.get("logging", "file_enabled"));
    if (t.has("logging", "file"))    opt.log_file    = t.get("logging", "file");
    if (t.has("logging", "console")) opt.log_console = parse_bool(t.get("logging", "console"));

    return opt;
}

} // namespace discord_tunnel
