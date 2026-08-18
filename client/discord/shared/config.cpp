#include "discord/shared/config.h"

#include <Windows.h>

#include <fstream>
#include <sstream>

namespace discord_client {
namespace {

std::wstring trim(const std::wstring& value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return L"";
    }
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::wstring widen(const std::string& value) {
    if (value.empty()) {
        return L"";
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string narrow(const std::wstring& value) {
    if (value.empty()) {
        return "";
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

}  // namespace

std::optional<AppConfig> load_config(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return std::nullopt;
    }

    AppConfig config;
    std::string line;
    while (std::getline(input, line)) {
        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        const std::wstring key = trim(widen(line.substr(0, pos)));
        const std::wstring value = trim(widen(line.substr(pos + 1)));
        if (key == L"server") {
            config.server = value;
        } else if (key == L"port") {
            try {
                const unsigned long parsed = std::stoul(value);
                if (parsed >= 1 && parsed <= 65535) {
                    config.port = static_cast<std::uint16_t>(parsed);
                }
            } catch (...) {
            }
        } else if (key == L"token") {
            config.token = value;
        } else if (key == L"ca_cert_path") {
            config.ca_cert_path = value;
        } else if (key == L"discord_root") {
            config.discord_root = value;
        } else if (key == L"skip_tls_verify") {
            config.skip_tls_verify = value == L"1" || value == L"true" || value == L"yes";
        }
    }

    if (config.server.empty() || config.token.empty()) {
        return std::nullopt;
    }
    return config;
}

bool save_config(const std::filesystem::path& path, const AppConfig& config) {
    std::ofstream output(path, std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }
    output << "server=" << narrow(config.server) << "\n";
    output << "port=" << config.port << "\n";
    output << "token=" << narrow(config.token) << "\n";
    output << "ca_cert_path=" << narrow(config.ca_cert_path) << "\n";
    output << "discord_root=" << narrow(config.discord_root) << "\n";
    output << "skip_tls_verify=" << (config.skip_tls_verify ? "1" : "0") << "\n";
    return output.good();
}

}  // namespace discord_client
