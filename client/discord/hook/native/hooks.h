#pragma once

namespace discord_tunnel::hooks {

// Install MinHook detours on WinSock + process functions and resolve
// pointers into discord_tunnel::real_io. Idempotent w.r.t. the underlying
// MinHook state.
void install();

// Tear everything down (MH_DisableHook + MH_Uninitialize). Called from
// DllMain on DLL_PROCESS_DETACH.
void uninstall();

// Build the cached command line that MyGetCommandLineW returns. Reads
// the live GetCommandLineW() and, if running under Discord with a proxy
// configured, appends `--proxy-server=...`.
void build_command_line_cache();

} // namespace discord_tunnel::hooks
