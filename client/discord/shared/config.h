#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace discord_client {

struct AppConfig {
    std::wstring server;
    std::uint16_t port = 443;
    std::wstring token;
    std::wstring ca_cert_path;
    std::wstring discord_root;
    bool skip_tls_verify = false;
};

std::optional<AppConfig> load_config(const std::filesystem::path& path);
bool save_config(const std::filesystem::path& path, const AppConfig& config);

}  // namespace discord_client
