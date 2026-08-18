#include "discord_dirs.h"

#include <windows.h>
#include <filesystem>

#include "config.h"
#include "logging.h"

namespace discord_tunnel::discord_dirs {

namespace {
constexpr std::wstring_view kNames[] = {
    L"Discord.exe",
    L"DiscordCanary.exe",
    L"DiscordPTB.exe",
    L"Update.exe",
};

bool case_insensitive_equal(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (towlower(a[i]) != towlower(b[i])) return false;
    }
    return true;
}

bool dir_contains_discord(const std::filesystem::path& dir) {
    for (auto name : kNames) {
        if (std::filesystem::exists(dir / name)) return true;
    }
    return false;
}
} // namespace

bool is_discord_executable_w(std::wstring_view filename) {
    for (auto name : kNames) {
        if (case_insensitive_equal(filename, name)) return true;
    }
    return false;
}

void copy_dll_into_all_app_dirs() {
    auto src_exe  = g_current_dir / L"discord-tunnel.exe";
    auto src_dll  = g_current_dir / L"version.dll";
    auto src_opts = g_current_dir / L"discord-tunnel.ini";
    if (!std::filesystem::exists(src_dll) || !std::filesystem::exists(src_opts)) return;

    auto base = g_current_dir.parent_path();
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(base, ec)) {
        if (!entry.is_directory()) continue;
        auto leaf = entry.path().filename().wstring();
        if (leaf.rfind(L"app-", 0) != 0) continue;
        auto& dir = entry.path();
        if (!dir_contains_discord(dir)) continue;

        auto dst_exe  = dir / L"discord-tunnel.exe";
        auto dst_dll  = dir / L"version.dll";
        auto dst_opts = dir / L"discord-tunnel.ini";

        bool copied_any = false;
        if (std::filesystem::exists(src_exe) && !std::filesystem::exists(dst_exe)) {
            ec.clear();
            copied_any = std::filesystem::copy_file(
                src_exe,
                dst_exe,
                std::filesystem::copy_options::overwrite_existing,
                ec
            ) || copied_any;
        }
        if (!std::filesystem::exists(dst_dll)) {
            ec.clear();
            copied_any = std::filesystem::copy_file(
                src_dll,
                dst_dll,
                std::filesystem::copy_options::overwrite_existing,
                ec
            ) || copied_any;
        }
        if (!std::filesystem::exists(dst_opts)) {
            ec.clear();
            copied_any = std::filesystem::copy_file(
                src_opts,
                dst_opts,
                std::filesystem::copy_options::overwrite_existing,
                ec
            ) || copied_any;
        }

        if (copied_any) {
            LOG_INFO("propagated tunnel into {}", dir.string());
        }
    }
}

} // namespace discord_tunnel::discord_dirs
