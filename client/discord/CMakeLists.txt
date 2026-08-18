cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(discord_tunnel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(DISCORD_RUNTIME_LIBRARY "" CACHE FILEPATH "Path to discord_runtime.lib built by Cargo")

if(NOT EXISTS "${DISCORD_RUNTIME_LIBRARY}")
    message(FATAL_ERROR "DISCORD_RUNTIME_LIBRARY does not exist: ${DISCORD_RUNTIME_LIBRARY}")
endif()

add_library(discord_runtime STATIC IMPORTED GLOBAL)
set_target_properties(discord_runtime PROPERTIES
    IMPORTED_LOCATION "${DISCORD_RUNTIME_LIBRARY}"
)

add_library(discord_shared STATIC
    shared/config.cpp
    shared/fs_utils.cpp
)

target_include_directories(discord_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(discord_shared PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

include(FetchContent)
FetchContent_Declare(
    MinHook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.4
)
FetchContent_MakeAvailable(MinHook)

add_executable(discord_tunnel_installer WIN32
    installer/main.cpp
)

target_include_directories(discord_tunnel_installer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(discord_tunnel_installer PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(discord_tunnel_installer PRIVATE
    discord_runtime
    discord_shared
    bcrypt
    comdlg32
    comctl32
    crypt32
    dwmapi
    ntdll
    secur32
    shell32
    userenv
    ws2_32
)
set_target_properties(discord_tunnel_installer PROPERTIES OUTPUT_NAME "discord-tunnel")

add_library(version SHARED
    hook/native/config.cpp
    hook/native/discord_dirs.cpp
    hook/native/dllmain.cpp
    hook/native/embedded_runtime.cpp
    hook/native/exports.cpp
    hook/native/hooks.cpp
    hook/native/logging.cpp
    hook/native/platform.cpp
    hook/native/proxy_value.cpp
    hook/native/real_io.cpp
    hook/native/socket_manager.cpp
    hook/native/socks5_udp.cpp
    hook/native/udp_strategy.cpp
)

target_include_directories(version PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/hook/native
)
target_compile_definitions(version PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(version PRIVATE
    discord_runtime
    minhook
    advapi32
    bcrypt
    crypt32
    iphlpapi
    ncrypt
    ntdll
    secur32
    shell32
    shlwapi
    user32
    userenv
    ws2_32
)

set_target_properties(version PROPERTIES
    OUTPUT_NAME version
    PREFIX ""
)
