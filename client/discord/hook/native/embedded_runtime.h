#pragma once

namespace discord_tunnel {

void start_embedded_runtime_async();
bool is_embedded_runtime_thread();

// Process-role helpers based on the Discord command line.
bool is_main_discord_process();
bool is_renderer_discord_process();
bool is_secondary_discord_process();

} // namespace discord_tunnel
