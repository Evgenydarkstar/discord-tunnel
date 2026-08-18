// Entry point. Order matters here:
//   1. Drop a marker file IMMEDIATELY. If you don't see discord-tunnel-attached.txt
//      next to Discord.exe, the DLL never ran - that's a "DLL hijack
//      didn't take" problem, not a discord_tunnel bug.
//   2. Load discord-tunnel.ini. We need it before Logger::init so the [logging]
//      section is honored.
//   3. Init Logger (optional file + maybe spawn console-setup thread).
//   4. Resolve the real version.dll trampolines.
//   5. Install hooks.
//
// AllocConsole is deliberately NOT called from DllMain itself - see
// platform.cpp::console_setup_thread for the loader-lock reasoning.

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>

#include "config.h"
#include "discord_dirs.h"
#include "embedded_runtime.h"
#include "hooks.h"
#include "logging.h"
#include "platform.h"

namespace {

std::filesystem::path module_directory(HMODULE module) {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(module, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

bool should_start_embedded_runtime() {
    // The main process and the renderer each run their own embedded runtime so
    // their in-process raw bridges work. The old SOCKS design only ran the
    // proxy in the main process, but raw bridges are process-local: the
    // renderer owns the voice UDP sockets, so without a runtime there every
    // WSASendTo fails with WSAECONNREFUSED -> NO_ROUTE. Other secondary
    // processes (gpu, utility) hold no UDP voice sockets and only run runtimes
    // that fail their QUIC handshake in sandboxed contexts, so they are
    // skipped.
    wchar_t exe[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto exe_name = std::filesystem::path(exe).filename().wstring();
    if (!discord_tunnel::discord_dirs::is_discord_executable_w(exe_name)) {
        return false;
    }
    return discord_tunnel::is_main_discord_process() ||
           discord_tunnel::is_renderer_discord_process();
}

// "Hey, I loaded!" - written to the Discord folder at the very first
// instruction of DllMain. If this file appears, the DLL hijack worked.
// If it doesn't, Discord is loading the system version.dll instead.
void write_attached_marker(const std::filesystem::path& dir) {
    FILE* f = nullptr;
    auto marker = (dir / "discord-tunnel-attached.txt").string();
    if (fopen_s(&f, marker.c_str(), "ab") != 0 || !f) return;

    std::time_t t = std::time(nullptr);
    std::tm tm = discord_tunnel::platform::localtime_safe(t);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    std::fprintf(f, "[%s] discord_tunnel attached  pid=%lu\n",
                 ts, static_cast<unsigned long>(GetCurrentProcessId()));
    std::fclose(f);
}

void log_state_summary() {
    const auto& opt = discord_tunnel::g_options;
    const auto& p = opt.proxy;
    if (opt.tunnel.enabled) {
        LOG_INFO("tunnel: {}:{} -> http://127.0.0.1:{}{}",
                 opt.tunnel.server,
                 opt.tunnel.port,
                 opt.tunnel.listen_port,
                 opt.tunnel.insecure ? " (insecure tls)" : "");
    }
    if (p.is_specified) {
        LOG_INFO("proxy: {}://{}{}:{}",
                  p.kind == discord_tunnel::ProxyKind::Socks5 ? "socks5" : "http",
                  p.has_auth() ? (p.login + ":***@") : std::string{},
                 p.host, p.port);
    } else {
        LOG_INFO("proxy: direct (UDP mangling only)");
    }
    LOG_INFO("udp strategy: {}{}",
             std::string{discord_tunnel::udp_strategy_name(opt.udp.strategy)},
             opt.udp.force_tcp_fallback ? " (overridden by force_tcp_fallback)" : "");
    LOG_INFO("udp bridge: raw loopback relay enabled");
}

// Runs in its own thread spawned after DllMain returns. Its job is to
// (a) trigger the actual AllocConsole, which is unsafe inside DllMain,
// and (b) re-emit the state summary so it shows up in the brand-new
// console window - the dllmain-time LOG_INFO calls happened before the
// console existed.
DWORD WINAPI deferred_console_init(LPVOID) {
    discord_tunnel::platform::enable_console();
    // Give the console thread a moment to flush its banner.
    Sleep(50);
    LOG_INFO("--- discord_tunnel console attached ---");
    log_state_summary();
    if (discord_tunnel::g_options.log_file_enabled) {
        LOG_INFO("Tip: the configured log file includes events that fired before the console opened.");
    }
    return 0;
}

void on_attach(HMODULE module) {
    auto current_dir = module_directory(module);

    // Step 1 - marker. Do this before ANYTHING that could fail.
    write_attached_marker(current_dir);

    discord_tunnel::g_current_dir = current_dir;

    // Step 2 - config. Must happen before Logger::init so [logging] applies.
    bool config_failed = false;
    std::string config_error;
    try {
        discord_tunnel::g_options = discord_tunnel::Config::load(current_dir / "discord-tunnel.ini");
    } catch (const std::exception& e) {
        config_failed = true;
        config_error = e.what();
    }

    // Step 3 - logger init. File output is optional. Console (if requested)
    // is marked as "wanted" but the actual AllocConsole happens later, off
    // the DllMain thread.
    discord_tunnel::Logger::init(current_dir);
    LOG_INFO("discord_tunnel dll attached at {}", current_dir.string());
    if (config_failed) {
        LOG_ERROR("config load failed: {}", config_error);
    }
    log_state_summary();

    // Step 4 - start the embedded runtime before hooks install so its own
    // transport sockets are not tracked and re-proxied by our WinSock hooks.
    // Runs in every Discord process (main + secondary), which is required for
    // per-process raw bridges (voice UDP lives in the renderer process).
    if (should_start_embedded_runtime()) {
        discord_tunnel::start_embedded_runtime_async();
    }

    // Step 5 - install hooks. The 17 version.dll exports are handled by
    // PE-level forwarders declared in src/exports.cpp, so we don't need
    // to resolve %SystemRoot%\System32\version.dll manually here.
    discord_tunnel::hooks::build_command_line_cache();
    discord_tunnel::hooks::install();
    LOG_INFO("hooks installed");

    // Step 6 - kick off console setup in a worker thread (no-op if the
    // user hasn't enabled it). Doing this last means hooks are active
    // by the time any Discord socket call hits.
    if (discord_tunnel::g_options.log_console) {
        HANDLE h = CreateThread(nullptr, 0, deferred_console_init, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
}

void on_detach() {
    LOG_INFO("discord_tunnel dll detaching");
    discord_tunnel::hooks::uninstall();
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        on_attach(module);
        break;
    case DLL_PROCESS_DETACH:
        on_detach();
        break;
    }
    return TRUE;
}
