#include "socket_manager.h"

namespace discord_tunnel {

SocketManager g_socket_manager;

void SocketManager::add(dt_socket_t sock, int sock_type, int protocol) {
    SocketEntry entry;
    entry.sock = sock;
    entry.is_tcp = (sock_type == SOCK_STREAM) &&
                   (protocol == IPPROTO_TCP || protocol == 0);
    entry.is_udp = (sock_type == SOCK_DGRAM) &&
                   (protocol == IPPROTO_UDP || protocol == 0);

    std::lock_guard<std::mutex> g(mutex_);
    for (auto& it : items_) {
        if (it.sock == sock) {
            it = entry;
            return;
        }
    }
    items_.push_back(entry);
}

void SocketManager::remove(dt_socket_t sock) {
    std::lock_guard<std::mutex> g(mutex_);
    for (auto it = items_.begin(); it != items_.end();) {
        if (it->sock == sock) it = items_.erase(it);
        else                  ++it;
    }
}

bool SocketManager::get(dt_socket_t sock, SocketEntry& out) const {
    std::lock_guard<std::mutex> g(mutex_);
    for (const auto& it : items_) {
        if (it.sock == sock) {
            out = it;
            return true;
        }
    }
    return false;
}

bool SocketManager::is_first_send(dt_socket_t sock, SocketEntry& out) {
    std::lock_guard<std::mutex> g(mutex_);
    for (auto& it : items_) {
        if (it.sock == sock && !it.has_sent) {
            it.has_sent = true;
            out = it;
            return true;
        }
    }
    return false;
}

void SocketManager::set_fake_http_proxy_flag(dt_socket_t sock) {
    std::lock_guard<std::mutex> g(mutex_);
    for (auto& it : items_) {
        if (it.sock == sock) {
            it.fake_http_proxy_flag = true;
            return;
        }
    }
}

bool SocketManager::reset_fake_http_proxy_flag(dt_socket_t sock) {
    std::lock_guard<std::mutex> g(mutex_);
    for (auto& it : items_) {
        if (it.sock == sock && it.fake_http_proxy_flag) {
            it.fake_http_proxy_flag = false;
            return true;
        }
    }
    return false;
}

} // namespace discord_tunnel
