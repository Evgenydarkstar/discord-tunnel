// Same role as the Pascal DiscordFolders.pas, but on the DLL side. The
// GUI installer has its own (richer) registry-based discovery; the DLL
// only needs to be able to spread itself to sibling app-* folders when
// Discord updates.

#pragma once

#include <string_view>

namespace discord_tunnel::discord_dirs {

bool is_discord_executable_w(std::wstring_view filename);

// Walk our own parent directory's siblings (..\app-*\), check whether
// they look like Discord folders, and drop version.dll + discord-tunnel.ini +
// discord-tunnel.exe into the ones that don't already have them.
void copy_dll_into_all_app_dirs();

} // namespace discord_tunnel::discord_dirs
