#include "logging.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>

#include "config.h"
#include "platform.h"

namespace discord_tunnel {

namespace {
    LogLevel g_level = LogLevel::Info;
    std::filesystem::path g_log_path;
    bool g_console_enabled = false;
    bool g_console_supports_color = false;
    std::mutex g_mutex;

    std::string timestamp() {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        auto tm = platform::localtime_safe(t);
        char buf[16];
        // Short HH:MM:SS - full date adds noise on the console.
        std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
        return buf;
    }

    std::string_view level_tag(LogLevel l) {
        switch (l) {
        case LogLevel::Off:   return "OFF  ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Trace: return "TRACE";
        }
        return "?    ";
    }

    // ANSI escape colors - see https://en.wikipedia.org/wiki/ANSI_escape_code
    std::string_view level_color(LogLevel l) {
        switch (l) {
        case LogLevel::Error: return "\x1b[31m"; // red
        case LogLevel::Warn:  return "\x1b[33m"; // yellow
        case LogLevel::Info:  return "\x1b[36m"; // cyan
        case LogLevel::Debug: return "\x1b[90m"; // bright black (gray)
        case LogLevel::Trace: return "\x1b[90m";
        default:              return "";
        }
    }
    constexpr std::string_view ANSI_RESET = "\x1b[0m";
}

LogLevel Logger::parse_level(std::string_view s) {
    std::string lower(s);
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "off")   return LogLevel::Off;
    if (lower == "error") return LogLevel::Error;
    if (lower == "warn" || lower == "warning") return LogLevel::Warn;
    if (lower == "info")  return LogLevel::Info;
    if (lower == "debug") return LogLevel::Debug;
    if (lower == "trace") return LogLevel::Trace;
    return LogLevel::Info;
}

LogLevel Logger::level() { return g_level; }

void Logger::init(const std::filesystem::path& exe_dir) {
    if (g_options.log_level) g_level = parse_level(*g_options.log_level);

    g_log_path.clear();
    if (g_options.log_file_enabled) {
        // Default to "discord-tunnel.log" if legacy configs do not specify a name.
        auto name = g_options.log_file.value_or(std::string{"discord-tunnel.log"});
        if (!name.empty()) {
            g_log_path = exe_dir / name;
        }
    }

    // Mark "we want console output" but DON'T trigger AllocConsole here.
    // The actual platform::enable_console() call happens off the DllMain
    // thread - see dllmain.cpp::deferred_console_init. Until that thread
    // finishes, console_write() falls back to OutputDebugString.
    g_console_enabled = g_options.log_console;
    g_console_supports_color = g_options.log_console; // optimistic; corrected once VT is probed
}

void Logger::log(LogLevel level, std::string_view msg) {
    if (level > g_level || g_level == LogLevel::Off) return;
    std::lock_guard<std::mutex> g(g_mutex);

    auto line = std::format("[{}] {} {}\n", timestamp(), level_tag(level), msg);

    // 1. Plain line to the log file.
    if (!g_log_path.empty()) {
        std::ofstream f(g_log_path, std::ios::app);
        if (f) f << line;
    }

    // 2. Colored line to the console (or stderr fallback).
    if (g_console_enabled) {
        if (g_console_supports_color) {
            auto colored = std::format("{}{}{}", level_color(level), line, ANSI_RESET);
            platform::console_write(colored.c_str());
        } else {
            platform::console_write(line.c_str());
        }
    }

    // 3. Always send to OutputDebugString / stderr too - costs nothing.
    platform::debug_write(line.c_str());
}

} // namespace discord_tunnel
