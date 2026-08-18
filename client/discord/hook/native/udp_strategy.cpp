// Pluggable UDP strategies. See udp_strategy.h for the design intent.
//
// All "real" UDP sends go through discord_tunnel::real_io::send_udp, which the
// per-OS shim wires to the unhooked OS function. That keeps this file
// completely platform-agnostic - it doesn't even know if it's running
// inside a Windows DLL or a Linux LD_PRELOAD library.

#include "udp_strategy.h"

#include <chrono>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

#include "config.h"
#include "logging.h"
#include "real_io.h"

namespace discord_tunnel {

namespace {

void send_raw(dt_socket_t sock, const std::uint8_t* data, std::size_t len,
              const sockaddr* to, dt_socklen_t to_len) {
    if (len == 0) return;
    real_io::send_udp(sock, data, len, to, to_len);
}

// One log line per new UDP voice connection - never per-packet.
// The `is_first_send` gate upstream ensures each strategy.on_first_send
// fires at most once per socket lifetime, so this is naturally rate-limited.
void log_first_send(const char* strategy_name, const UdpFirstSendCtx& ctx) {
    LOG_INFO("[udp] first send {} bytes -> {} (strategy={})",
             ctx.original_packet.size(),
             platform::format_sockaddr(ctx.to),
             strategy_name);
}

std::vector<std::uint8_t> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void emit_prefix_packets(const UdpFirstSendCtx& ctx,
                         const std::vector<std::vector<std::uint8_t>>& packets,
                         std::uint32_t delay_ms) {
    for (const auto& p : packets) {
        send_raw(ctx.sock, p.data(), p.size(), ctx.to, ctx.to_len);
    }
    if (delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
}

// --- Strategies ------------------------------------------------------------

class OffStrategy : public IUdpStrategy {
public:
    bool on_first_send(const UdpFirstSendCtx& ctx) override {
        log_first_send("off", ctx);
        return true;
    }
};

// Pascal-compatible default: discord-tunnel-packet.bin (if present) + 0x00 + 0x01 + sleep(50).
class ClassicStrategy : public IUdpStrategy {
public:
    bool on_first_send(const UdpFirstSendCtx& ctx) override {
        if (ctx.original_packet.size() != 74) return true;
        log_first_send("classic", ctx);

        auto bin = read_file_bytes(g_current_dir / "discord-tunnel-packet.bin");
        if (!bin.empty()) {
            LOG_DEBUG("[udp] sending discord-tunnel-packet.bin ({} bytes) as prefix", bin.size());
            send_raw(ctx.sock, bin.data(), bin.size(), ctx.to, ctx.to_len);
        }
        std::uint8_t b = 0x00;
        send_raw(ctx.sock, &b, 1, ctx.to, ctx.to_len);
        b = 0x01;
        send_raw(ctx.sock, &b, 1, ctx.to, ctx.to_len);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return true;
    }
};

// Generic configurable strategy fed by discord-tunnel.ini.
class ConfiguredStrategy : public IUdpStrategy {
public:
    explicit ConfiguredStrategy(const UdpSettings& s) : settings_(s) {}

    bool on_first_send(const UdpFirstSendCtx& ctx) override {
        if (ctx.original_packet.size() != 74) return true;
        log_first_send(settings_.split_first > 1 ? "split" : "custom", ctx);
        emit_prefix_packets(ctx, settings_.prefix_packets, settings_.prefix_delay_ms);

        if (settings_.split_first > 1 && !ctx.original_packet.empty()) {
            // Split the original packet into N near-equal chunks, then
            // suppress the real send (we already emitted the bytes).
            std::size_t n = settings_.split_first;
            std::size_t total = ctx.original_packet.size();
            std::size_t base = total / n;
            std::size_t rem  = total % n;
            std::size_t offset = 0;
            for (std::size_t i = 0; i < n; ++i) {
                std::size_t this_len = base + (i < rem ? 1 : 0);
                send_raw(ctx.sock, ctx.original_packet.data() + offset, this_len,
                         ctx.to, ctx.to_len);
                offset += this_len;
            }
            return false;
        }

        return true;
    }

private:
    UdpSettings settings_;
};

// Curated payloads - real bytes live in dist/strategies/.
class CuratedStrategy : public IUdpStrategy {
public:
    explicit CuratedStrategy(const char* preset_filename) : preset_(preset_filename) {}

    bool on_first_send(const UdpFirstSendCtx& ctx) override {
        if (ctx.original_packet.size() != 74) return true;
        log_first_send(preset_, ctx);
        auto bytes = read_file_bytes(g_current_dir / preset_);
        if (bytes.empty()) {
            LOG_WARN("[udp] curated preset '{}' is empty or missing; no prefix sent", preset_);
        } else {
            LOG_DEBUG("[udp] sending curated preset '{}' ({} bytes)", preset_, bytes.size());
            send_raw(ctx.sock, bytes.data(), bytes.size(), ctx.to, ctx.to_len);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return true;
    }

private:
    const char* preset_;
};

// ISSUE-65-IDEAS.md section 2.6: intentionally torch the first packet so Discord
// times out the UDP medium quickly and falls back to TCP/TURNS.
class ForceTcpFallbackStrategy : public IUdpStrategy {
public:
    bool on_first_send(const UdpFirstSendCtx& ctx) override {
        if (ctx.original_packet.size() != 74) return true;
        log_first_send("force_tcp_fallback", ctx);
        std::uint8_t bogus[8] = {0};
        send_raw(ctx.sock, bogus, sizeof(bogus), ctx.to, ctx.to_len);
        return false; // drop the real packet
    }
};

} // namespace

std::unique_ptr<IUdpStrategy> make_strategy(const UdpSettings& s) {
    if (s.force_tcp_fallback) {
        LOG_INFO("UDP strategy: force_tcp_fallback (overrides 'strategy')");
        return std::make_unique<ForceTcpFallbackStrategy>();
    }
    switch (s.strategy) {
    case UdpStrategy::Off:    return std::make_unique<OffStrategy>();
    case UdpStrategy::UaeV1:  return std::make_unique<CuratedStrategy>("uae-v1.bin");
    case UdpStrategy::UaeV2:  return std::make_unique<CuratedStrategy>("uae-v2.bin");
    case UdpStrategy::Custom: return std::make_unique<ConfiguredStrategy>(s);
    case UdpStrategy::Split: {
        auto cfg = s;
        if (cfg.split_first < 2) cfg.split_first = 2;
        return std::make_unique<ConfiguredStrategy>(cfg);
    }
    case UdpStrategy::Classic:
    default:                  return std::make_unique<ClassicStrategy>();
    }
}

} // namespace discord_tunnel
