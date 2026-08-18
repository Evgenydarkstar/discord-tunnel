#include "discord/shared/config.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace discord_client {
namespace {

using SectionMap = std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::wstring>>;

std::wstring trim(const std::wstring& value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return L"";
    }
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::wstring to_lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(static_cast<wint_t>(ch)));
    });
    return value;
}

bool parse_bool(const std::wstring& value) {
    const auto lower = to_lower(trim(value));
    return lower == L"1" || lower == L"true" || lower == L"yes" || lower == L"on";
}

SectionMap parse_ini(const std::filesystem::path& path) {
    SectionMap sections;
    std::ifstream input(path);
    if (!input.is_open()) {
        return sections;
    }

    std::wstring section;
    std::string line;
    while (std::getline(input, line)) {
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }

        const std::wstring wide = trim(utf8_to_wide(line));
        if (wide.empty() || wide[0] == L';' || wide[0] == L'#') {
            continue;
        }
        if (wide.front() == L'[' && wide.back() == L']') {
            section = to_lower(trim(wide.substr(1, wide.size() - 2)));
            continue;
        }

        const auto pos = wide.find(L'=');
        if (pos == std::wstring::npos) {
            continue;
        }
        const auto key = to_lower(trim(wide.substr(0, pos)));
        const auto value = trim(wide.substr(pos + 1));
        sections[section][key] = value;
    }
    return sections;
}

std::wstring get_value(const SectionMap& sections,
                       const std::wstring& section,
                       const std::wstring& key,
                       const std::wstring& fallback_section = L"",
                       const std::wstring& fallback_key = L"") {
    const auto sec_it = sections.find(section);
    if (sec_it != sections.end()) {
        const auto key_it = sec_it->second.find(key);
        if (key_it != sec_it->second.end()) {
            return key_it->second;
        }
    }
    if (!fallback_key.empty()) {
        return get_value(sections, fallback_section, fallback_key);
    }
    return L"";
}

}  // namespace

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) {
        return L"";
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string wide_to_utf8(const std::wstring& value) {
    if (value.empty()) {
        return "";
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::optional<AppConfig> load_config(const std::filesystem::path& path) {
    const auto sections = parse_ini(path);
    if (sections.empty()) {
        return std::nullopt;
    }

    AppConfig config;

    config.server = get_value(sections, L"tunnel", L"server", L"", L"server");
    config.token = get_value(sections, L"tunnel", L"token", L"", L"token");
    config.ca_cert_path = get_value(sections, L"tunnel", L"ca_cert", L"", L"ca_cert_path");
    config.discord_root = get_value(sections, L"client", L"discord_root", L"", L"discord_root");

    const auto port_value = get_value(sections, L"tunnel", L"port", L"", L"port");
    if (!port_value.empty()) {
        try {
            const unsigned long parsed = std::stoul(port_value);
            if (parsed >= 1 && parsed <= 65535) {
                config.port = static_cast<std::uint16_t>(parsed);
            }
        } catch (...) {
        }
    }

    const auto insecure_value = get_value(sections, L"tunnel", L"insecure", L"", L"skip_tls_verify");
    if (!insecure_value.empty()) {
        config.skip_tls_verify = parse_bool(insecure_value);
    }

    if (config.server.empty() || config.token.empty() || config.ca_cert_path.empty()) {
        return std::nullopt;
    }
    return config;
}

bool save_config(const std::filesystem::path& path, const AppConfig& config) {
    std::ofstream output(path, std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }
    output << "[tunnel]\n";
    output << "enabled=1\n";
    output << "server=" << wide_to_utf8(config.server) << "\n";
    output << "port=" << config.port << "\n";
    output << "token=" << wide_to_utf8(config.token) << "\n";
    output << "ca_cert=" << wide_to_utf8(config.ca_cert_path) << "\n";
    output << "insecure=" << (config.skip_tls_verify ? "1" : "0") << "\n";
    output << "listen_port=17821\n\n";
    output << "[client]\n";
    output << "discord_root=" << wide_to_utf8(config.discord_root) << "\n";
    return output.good();
}

}  // namespace discord_client
