#include "platform.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#if DT_OS_WIN
#  include <io.h>
#  include <process.h>
#else
#  include <arpa/inet.h>
#endif

namespace discord_tunnel::platform {

namespace {
    std::atomic_bool g_console_attached{false};
    std::atomic_bool g_console_supports_vt{false};

#if DT_OS_WIN
    // The actual AllocConsole work. Runs in its own thread because doing
    // it directly from DllMain triggers loader-lock deadlocks: AllocConsole
    // internally spins up conhost.exe and loads more DLLs, which is
    // explicitly unsafe during DLL_PROCESS_ATTACH.
    DWORD WINAPI console_setup_thread(LPVOID) {
        if (!AllocConsole()) {
            // We may already have a console (e.g. Discord launched from cmd).
            AttachConsole(ATTACH_PARENT_PROCESS);
        }
        SetConsoleTitleW(L"Discord Tunnel - debug console");

        FILE* dummy = nullptr;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
        freopen_s(&dummy, "CONIN$",  "r", stdin);

        // Try to turn on VT processing for ANSI color escape codes.
        // Available on Windows 10 1607+; older systems silently fail.
        bool vt = false;
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
            if (SetConsoleMode(h, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */)) {
                vt = true;
            }
        }

        g_console_supports_vt.store(vt);
        // Publish "attached" only AFTER the streams are usable.
        g_console_attached.store(true);

        std::fputs(vt ? "\x1b[36m[discord_tunnel] console attached.\x1b[0m\n"
                      : "[discord_tunnel] console attached.\n", stdout);
        std::fflush(stdout);
        return 0;
    }
#endif
} // anonymous

void debug_write(const char* line) {
#if DT_OS_WIN
    OutputDebugStringA(line);
#else
    std::fputs(line, stderr);
#endif
}

bool enable_console() {
    if (g_console_attached.load()) {
        return g_console_supports_vt.load();
    }

#if DT_OS_WIN
    // Defer to a worker thread. We can't AllocConsole from DllMain.
    HANDLE h = CreateThread(nullptr, 0, console_setup_thread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    // Optimistically claim VT support so loggers prepare colored output.
    // Worst case on a pre-Win10 box: a few raw "\x1b[" sequences in the
    // output. Better than no logs.
    return true;
#else
    g_console_attached.store(true);
    g_console_supports_vt.store(true);
    return true;
#endif
}

void console_write(const char* line) {
    if (g_console_attached.load()) {
        std::fputs(line, stdout);
        std::fflush(stdout);
        return;
    }
    // Console not ready yet (worker thread still racing in). Spill into
    // OutputDebugString - DbgView/Visual Studio captures these - and
    // the log file already has everything anyway.
    debug_write(line);
}

std::string format_sockaddr(const sockaddr* sa) {
    if (!sa || sa->sa_family != AF_INET) return "?";
    auto in = reinterpret_cast<const sockaddr_in*>(sa);
    char ipbuf[INET_ADDRSTRLEN] = {0};
#if DT_OS_WIN
    inet_ntop(AF_INET, const_cast<IN_ADDR*>(&in->sin_addr), ipbuf, INET_ADDRSTRLEN);
#else
    inet_ntop(AF_INET, &in->sin_addr, ipbuf, INET_ADDRSTRLEN);
#endif
    unsigned port = ntohs(in->sin_port);
    char out[64];
    std::snprintf(out, sizeof(out), "%s:%u", ipbuf, port);
    return out;
}

} // namespace discord_tunnel::platform
