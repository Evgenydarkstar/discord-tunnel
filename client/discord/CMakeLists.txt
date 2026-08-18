cmake_minimum_required(VERSION 3.20)
project(discord_tunnel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(discord_shared STATIC
    shared/config.cpp
    shared/fs_utils.cpp
)

target_include_directories(discord_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(discord_shared PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(discord_tunnel_installer WIN32
    installer/main.cpp
)

target_include_directories(discord_tunnel_installer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(discord_tunnel_installer PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(discord_tunnel_installer PRIVATE discord_shared comctl32 shell32)
set_target_properties(discord_tunnel_installer PROPERTIES OUTPUT_NAME "discord-tunnel")

add_library(version SHARED
    hook/version_proxy.cpp
)

target_include_directories(version PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(version PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(version PRIVATE discord_shared shell32)

set_target_properties(version PROPERTIES
    OUTPUT_NAME version
    PREFIX ""
)
