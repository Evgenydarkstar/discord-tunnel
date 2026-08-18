// Tiny header-only-ish logger. Lives in the DLL itself so we don't drag
// fmt/spdlog into a 32-bit hot-loaded library.

#pragma once

#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <string_view>

namespace discord_tunnel {

enum class LogLevel { Off, Error, Warn, Info, Debug, Trace };

class Logger {
public:
    static void init(const std::filesystem::path& exe_dir);
    static void log(LogLevel level, std::string_view msg);

    static LogLevel level();

    static LogLevel parse_level(std::string_view s);
};

#define LOG_AT(LEVEL, ...) \
    do { \
        if (::discord_tunnel::Logger::level() >= ::discord_tunnel::LogLevel::LEVEL) { \
            ::discord_tunnel::Logger::log(::discord_tunnel::LogLevel::LEVEL, std::format(__VA_ARGS__)); \
        } \
    } while (0)

#define LOG_ERROR(...) LOG_AT(Error, __VA_ARGS__)
#define LOG_WARN(...)  LOG_AT(Warn,  __VA_ARGS__)
#define LOG_INFO(...)  LOG_AT(Info,  __VA_ARGS__)
#define LOG_DEBUG(...) LOG_AT(Debug, __VA_ARGS__)
#define LOG_TRACE(...) LOG_AT(Trace, __VA_ARGS__)

} // namespace discord_tunnel
