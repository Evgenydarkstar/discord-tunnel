#include "embedded_runtime.h"

#include <windows.h>

#include <atomic>
#include <cwctype>
#include <mutex>
#include <string>
#include <string_view>

#include "config.h"
#include "logging.h"
#include "runtime_ffi.h"

namespace discord_tunnel {

namespace {

std::once_flag g_runtime_once;
std::atomic<DWORD> g_runtime_thread_id{0};

std::string path_to_utf8(const std::filesystem::path& path) {
    auto u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.c_str()), u8.size());
}

std::filesystem::path resolve_ca_path(const std::filesystem::path& path) {
    if (path.is_absolute()) {
        return path;
    }
    return g_current_dir / path;
}

DWORD WINAPI runtime_thread(LPVOID) {
    g_runtime_thread_id.store(GetCurrentThreadId(), std::memory_order_release);
    const auto& tunnel = g_options.tunnel;
    const std::string ca_pem = tunnel.ca_cert.has_value()
        ? path_to_utf8(resolve_ca_path(*tunnel.ca_cert))
        : std::string{};
    const uint16_t listen_port = is_secondary_discord_process() ? 0 : tunnel.listen_port;

    LOG_INFO("starting embedded runtime via {}:{} with local raw bridge base {}",
             tunnel.server, tunnel.port, listen_port);

    const int rc = dt_embedded_start(tunnel.server.c_str(),
                                     tunnel.port,
                                     tunnel.token.c_str(),
                                     ca_pem.empty() ? nullptr : ca_pem.c_str(),
                                     tunnel.insecure ? 1 : 0,
                                     listen_port);
    switch (rc) {
    case DT_OK:
        LOG_INFO("embedded runtime started");
        break;
    case DT_ERR_TRANSPORT_AUTH:
        LOG_ERROR("embedded runtime auth failed");
        break;
    case DT_ERR_TRANSPORT:
        LOG_ERROR("embedded runtime transport startup failed");
        break;
    default:
        LOG_ERROR("embedded runtime startup failed with code {}", rc);
        break;
    }
    g_runtime_thread_id.store(0, std::memory_order_release);
    return 0;
}

} // namespace

bool has_command_line_flag(const wchar_t* needle) {
    if (!needle) {
        return false;
    }
    const wchar_t* raw_cmd = GetCommandLineW();
    if (!raw_cmd) {
        return false;
    }
    const std::wstring_view cmd(raw_cmd);
    const std::wstring_view flag(needle);
    if (flag.empty() || cmd.size() < flag.size()) {
        return false;
    }
    for (size_t i = 0; i + flag.size() <= cmd.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < flag.size(); ++j) {
            if (towlower(cmd[i + j]) != towlower(flag[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

// The main process has no --type= flag; every secondary Discord process
// (--type=renderer, --type=utility, --type=gpu-process, ...) does.
bool is_main_discord_process() {
    return !has_command_line_flag(L"--type=");
}

// The renderer owns Discord's voice/WebRTC UDP sockets, so it is the only
// secondary process that needs its own runtime (for per-process raw bridges).
bool is_renderer_discord_process() {
    return has_command_line_flag(L"--type=renderer");
}

// Secondary Discord processes run their own runtime too, but must NOT bind
// the shared TCP listen port used by the main process's HTTP proxy. An
// ephemeral port keeps the HTTP listener (which nobody uses in secondary
// processes) from colliding with the main process's 127.0.0.1:listen_port
// proxy.
bool is_secondary_discord_process() {
    return has_command_line_flag(L"--type=");
}

void start_embedded_runtime_async() {
    if (!g_options.tunnel.enabled) {
        LOG_INFO("embedded runtime disabled in [tunnel]");
        return;
    }
    if (g_options.tunnel.server.empty() || g_options.tunnel.token.empty()) {
        LOG_ERROR("embedded runtime not started: [tunnel] server/token missing");
        return;
    }

    std::call_once(g_runtime_once, []() {
        HANDLE h = CreateThread(nullptr, 0, runtime_thread, nullptr, 0, nullptr);
        if (h) {
            CloseHandle(h);
        } else {
            LOG_ERROR("failed to create embedded runtime thread: {}", GetLastError());
        }
    });
}

bool is_embedded_runtime_thread() {
    return g_runtime_thread_id.load(std::memory_order_acquire) == GetCurrentThreadId();
}

} // namespace discord_tunnel
