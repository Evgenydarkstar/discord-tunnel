#include "discord/shared/fs_utils.h"

#include <Windows.h>
#include <shlobj.h>

#include <algorithm>
#include <system_error>

namespace discord_client {

std::filesystem::path current_module_path() {
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(std::wstring(buffer, length));
}

std::filesystem::path default_discord_root() {
    wchar_t buffer[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buffer))) {
        const auto base = std::filesystem::path(buffer);
        const auto discord = base / L"Discord";
        if (std::filesystem::is_directory(discord)) {
            return discord;
        }
    }
    return {};
}

std::vector<std::filesystem::path> find_discord_app_dirs(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> results;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return results;
    }
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (!entry.is_directory()) {
            continue;
        }
        const auto name = entry.path().filename().wstring();
        if (name.rfind(L"app-", 0) == 0) {
            results.push_back(entry.path());
        }
    }
    std::sort(results.begin(), results.end());
    return results;
}

std::wstring join_lines(const std::vector<std::wstring>& lines) {
    std::wstring out;
    for (size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size()) {
            out += L"\r\n";
        }
    }
    return out;
}

bool copy_file_overwrite(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::create_directories(to.parent_path(), ec);
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
    return !ec;
}

bool remove_if_exists(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return true;
    }
    return std::filesystem::remove(path, ec);
}

std::filesystem::path config_path_for_app_dir(const std::filesystem::path& app_dir) {
    return app_dir / L"discord-tunnel.ini";
}

std::filesystem::path helper_path_for_app_dir(const std::filesystem::path& app_dir) {
    return app_dir / L"discord-tunnel.exe";
}

std::filesystem::path dll_path_for_app_dir(const std::filesystem::path& app_dir) {
    return app_dir / L"version.dll";
}

std::filesystem::path log_path_for_app_dir(const std::filesystem::path& app_dir) {
    return app_dir / L"discord-tunnel.log";
}

}  // namespace discord_client
