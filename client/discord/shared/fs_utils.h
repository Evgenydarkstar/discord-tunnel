#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace discord_client {

std::filesystem::path current_module_path();
std::filesystem::path default_discord_root();
std::vector<std::filesystem::path> find_discord_app_dirs(const std::filesystem::path& root);
std::wstring join_lines(const std::vector<std::wstring>& lines);
bool copy_file_overwrite(const std::filesystem::path& from, const std::filesystem::path& to);
bool remove_if_exists(const std::filesystem::path& path);
std::filesystem::path config_path_for_app_dir(const std::filesystem::path& app_dir);
std::filesystem::path helper_path_for_app_dir(const std::filesystem::path& app_dir);
std::filesystem::path dll_path_for_app_dir(const std::filesystem::path& app_dir);
std::filesystem::path log_path_for_app_dir(const std::filesystem::path& app_dir);

}  // namespace discord_client
