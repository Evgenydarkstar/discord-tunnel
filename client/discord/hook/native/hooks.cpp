// WinSock + process hooks. The bridge between the user-facing discord-tunnel.ini
// knobs and the OS calls Discord actually makes.
//
// Hook surface keeps native WinSock semantics but redirects public Discord
// sockets into a local raw bridge backed by the Rust QUIC/H3 runtime.
//
//   socket / WSASocket    - track which sockets are TCP vs UDP.
//   connect               - opens a per-socket raw bridge and connects TCP
//                           sockets to loopback instead of the public target.
//   sendto / WSASendTo    - redirect UDP payloads to a per-socket raw bridge.
//   send / WSASend        - same for connected UDP sockets.
//   recvfrom / WSARecvFrom / WSARecvMsg - rewrite loopback source addresses
//                           back to the original public peer.
//   SetFileCompletionNotificationModes - keep libuv on hooked receive APIs.
//   CreateProcessW        - copy version.dll/discord-tunnel.ini into freshly-updated
//                           Discord folders so the next launch keeps working.
//
// Heavy lifting lives in the per-feature modules (`udp_strategy.cpp`,
// `socks5_udp.cpp`). If a feature here looks suspicious, cross-reference
// the original Pascal `legacy-client.dpr` from upstream - that file is the
// canonical reference for the HTTP/SOCKS5 wire format we're targeting.

#include "hooks.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mswsock.h>
#include <MinHook.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "discord_dirs.h"
#include "embedded_runtime.h"
#include "logging.h"
#include "real_io.h"
#include "runtime_ffi.h"
#include "socket_manager.h"

namespace discord_tunnel::hooks {

namespace {

// ---- Real function pointers, populated by MinHook --------------------------

using Socket_t  = SOCKET (WSAAPI*)(int, int, int);
using Connect_t = int (WSAAPI*)(SOCKET, const sockaddr*, int);
using Getpeername_t = int (WSAAPI*)(SOCKET, sockaddr*, int*);
using WSASocket_t = SOCKET (WSAAPI*)(int, int, int, LPWSAPROTOCOL_INFOW, GROUP, DWORD);
using WSASend_t = int (WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD,
                                LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using WSASendTo_t = int (WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD,
                                  const sockaddr*, int,
                                  LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using Send_t    = int (WSAAPI*)(SOCKET, const char*, int, int);
using Recv_t    = int (WSAAPI*)(SOCKET, char*, int, int);
using WSARecv_t = int (WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD,
                                LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using Closesocket_t = int (WSAAPI*)(SOCKET);
using Sendto_t  = int (WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
using Recvfrom_t = int (WSAAPI*)(SOCKET, char*, int, int, sockaddr*, int*);
using WSARecvFrom_t = int (WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD,
                                    sockaddr*, LPINT, LPWSAOVERLAPPED,
                                    LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using WSAIoctl_t = int (WSAAPI*)(SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD,
                                 LPDWORD, LPWSAOVERLAPPED,
                                 LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using WSARecvMsg_t = INT (*)(SOCKET, LPWSAMSG, LPDWORD, LPWSAOVERLAPPED,
                             LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using WSAGetOverlappedResult_t = BOOL (WSAAPI*)(SOCKET, LPWSAOVERLAPPED, LPDWORD, BOOL, LPDWORD);
using GetCommandLineW_t = LPWSTR (WINAPI*)();
using GetEnvironmentVariableW_t = DWORD (WINAPI*)(LPCWSTR, LPWSTR, DWORD);
using GetQueuedCompletionStatus_t = BOOL (WINAPI*)(HANDLE, LPDWORD, PULONG_PTR, LPOVERLAPPED*, DWORD);
using GetQueuedCompletionStatusEx_t = BOOL (WINAPI*)(HANDLE, LPOVERLAPPED_ENTRY, ULONG, PULONG, DWORD, BOOL);
using SetFileCompletionNotificationModes_t = BOOL (WINAPI*)(HANDLE, UCHAR);
using CreateProcessW_t  = BOOL (WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
                                         LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID,
                                         LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);

Socket_t   real_socket = nullptr;
Connect_t  real_connect = nullptr;
Getpeername_t real_getpeername = nullptr;
WSASocket_t real_wsa_socket = nullptr;
WSASend_t  real_wsa_send = nullptr;
WSASendTo_t real_wsa_sendto = nullptr;
Send_t     real_send = nullptr;
Recv_t     real_recv = nullptr;
WSARecv_t  real_wsa_recv = nullptr;
Closesocket_t real_closesocket = nullptr;
Sendto_t   real_sendto = nullptr;
Recvfrom_t real_recvfrom = nullptr;
WSARecvFrom_t real_wsa_recvfrom = nullptr;
WSAIoctl_t real_wsa_ioctl = nullptr;
WSARecvMsg_t real_wsa_recv_msg = nullptr;
WSAGetOverlappedResult_t real_wsa_get_overlapped_result = nullptr;
GetCommandLineW_t          real_get_cmdline = nullptr;
GetEnvironmentVariableW_t  real_get_env = nullptr;
GetQueuedCompletionStatus_t real_get_queued_completion_status = nullptr;
GetQueuedCompletionStatusEx_t real_get_queued_completion_status_ex = nullptr;
SetFileCompletionNotificationModes_t real_set_file_completion_notification_modes = nullptr;
CreateProcessW_t           real_create_process = nullptr;

std::wstring g_cmdline_cache;
std::atomic<unsigned> g_udp_api_logs{0};
std::atomic<unsigned> g_udp_completion_logs{0};
std::atomic<unsigned> g_udp_receive_logs{0};
std::atomic<unsigned> g_libuv_receive_logs{0};

struct BridgeBinding {
    uint64_t bridge_id = 0;
    bool udp = false;
    sockaddr_storage original_peer{};
    int original_peer_len = 0;
    sockaddr_in loopback{};
};

struct PendingUdpRecv {
    SOCKET sock = INVALID_SOCKET;
    char* app_buffer = nullptr;
    ULONG app_capacity = 0;
    sockaddr* from = nullptr;
    LPINT fromlen = nullptr;
    LPDWORD flags = nullptr;
    LPWSAOVERLAPPED_COMPLETION_ROUTINE completion = nullptr;
    bool completion_wrapped = false;
    LPWSAMSG app_message = nullptr;
    WSAMSG relay_message{};
    bool finished = false;
    DWORD final_bytes = 0;
};

struct PendingUdpSend {
    SOCKET sock = INVALID_SOCKET;
    DWORD app_bytes = 0;
    LPWSAOVERLAPPED_COMPLETION_ROUTINE completion = nullptr;
};

std::mutex g_pending_udp_mutex;
std::unordered_map<LPWSAOVERLAPPED, std::shared_ptr<PendingUdpRecv>> g_pending_udp_recvs;
std::unordered_map<LPWSAOVERLAPPED, std::shared_ptr<PendingUdpSend>> g_pending_udp_sends;
std::unordered_map<SOCKET, std::pair<sockaddr_storage, int>> g_udp_connected_peers;
std::unordered_map<SOCKET, BridgeBinding> g_socket_bridges;

bool is_embedded_transport_destination(const sockaddr* to);
bool is_loopback_destination(const sockaddr* to);

bool bridge_ready() {
    return dt_bridge_is_ready() != 0;
}

std::wstring widen_utf8(std::string_view text) {
    if (text.empty()) return {};
    int required = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), required);
    return wide;
}

DWORD write_env_override(std::wstring_view value, LPWSTR buf, DWORD size) {
    DWORD required_without_null = static_cast<DWORD>(value.size());
    if (!buf || size == 0) {
        return required_without_null + 1;
    }
    if (size <= required_without_null) {
        return required_without_null + 1;
    }
    std::wmemcpy(buf, value.data(), value.size());
    buf[value.size()] = L'\0';
    return required_without_null;
}

bool sockaddr_to_host_port(const sockaddr* addr, int addr_len, std::string& host, uint16_t& port) {
    if (!addr || addr_len <= 0) return false;
    char buffer[INET6_ADDRSTRLEN] = {};
    switch (addr->sa_family) {
    case AF_INET: {
        if (addr_len < static_cast<int>(sizeof(sockaddr_in))) return false;
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(addr);
        if (!InetNtopA(AF_INET, const_cast<IN_ADDR*>(&ipv4->sin_addr), buffer, sizeof(buffer))) {
            return false;
        }
        host = buffer;
        port = ntohs(ipv4->sin_port);
        return port != 0;
    }
    case AF_INET6: {
        if (addr_len < static_cast<int>(sizeof(sockaddr_in6))) return false;
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(addr);
        if (!InetNtopA(AF_INET6, const_cast<IN6_ADDR*>(&ipv6->sin6_addr), buffer, sizeof(buffer))) {
            return false;
        }
        host = buffer;
        port = ntohs(ipv6->sin6_port);
        return port != 0;
    }
    default:
        return false;
    }
}

sockaddr_in make_loopback(uint16_t port) {
    sockaddr_in loopback{};
    loopback.sin_family = AF_INET;
    loopback.sin_port = htons(port);
    loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return loopback;
}

bool is_same_peer(const BridgeBinding& binding, const sockaddr* addr, int addr_len) {
    return binding.original_peer_len == addr_len &&
        std::memcmp(&binding.original_peer, addr, static_cast<std::size_t>(addr_len)) == 0;
}

void rewrite_source_from_bridge(SOCKET sock, sockaddr* from, int* fromlen) {
    if (!from || !fromlen) return;
    std::lock_guard<std::mutex> g(g_pending_udp_mutex);
    auto it = g_socket_bridges.find(sock);
    if (it == g_socket_bridges.end()) return;
    const auto& binding = it->second;
    if (!binding.udp || *fromlen < binding.original_peer_len) return;
    std::memcpy(from, &binding.original_peer, static_cast<std::size_t>(binding.original_peer_len));
    *fromlen = binding.original_peer_len;
}

bool get_bridge_binding(SOCKET sock, BridgeBinding& binding) {
    std::lock_guard<std::mutex> g(g_pending_udp_mutex);
    auto it = g_socket_bridges.find(sock);
    if (it == g_socket_bridges.end()) return false;
    binding = it->second;
    return true;
}

void close_bridge_binding(SOCKET sock) {
    uint64_t bridge_id = 0;
    {
        std::lock_guard<std::mutex> g(g_pending_udp_mutex);
        auto it = g_socket_bridges.find(sock);
        if (it == g_socket_bridges.end()) return;
        bridge_id = it->second.bridge_id;
        g_socket_bridges.erase(it);
    }
    if (bridge_id != 0) {
        dt_bridge_close(bridge_id);
    }
}

bool open_bridge_for_socket(SOCKET sock, const sockaddr* addr, int addr_len, bool udp, BridgeBinding& binding) {
    std::string host;
    uint16_t port = 0;
    if (!sockaddr_to_host_port(addr, addr_len, host, port)) return false;
    uint64_t bridge_id = 0;
    uint16_t loopback_port = 0;
    int rc = udp
        ? dt_bridge_udp_open(host.c_str(), port, &bridge_id, &loopback_port)
        : dt_bridge_tcp_open(host.c_str(), port, &bridge_id, &loopback_port);
    if (rc != DT_OK || bridge_id == 0 || loopback_port == 0) {
        return false;
    }

    BridgeBinding next{};
    next.bridge_id = bridge_id;
    next.udp = udp;
    next.original_peer_len = addr_len;
    std::memcpy(&next.original_peer, addr, static_cast<std::size_t>(addr_len));
    next.loopback = make_loopback(loopback_port);

    {
        std::lock_guard<std::mutex> g(g_pending_udp_mutex);
        g_socket_bridges[sock] = next;
    }
    binding = next;
    return true;
}

bool ensure_bridge_binding(SOCKET sock, const sockaddr* addr, int addr_len, bool udp, BridgeBinding& binding) {
    if (!addr || addr_len <= 0 || is_loopback_destination(addr) || is_embedded_transport_destination(addr)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> g(g_pending_udp_mutex);
        auto it = g_socket_bridges.find(sock);
        if (it != g_socket_bridges.end() && it->second.udp == udp && is_same_peer(it->second, addr, addr_len)) {
            binding = it->second;
            return true;
        }
    }
    close_bridge_binding(sock);
    return open_bridge_for_socket(sock, addr, addr_len, udp, binding);
}

bool get_udp_connected_peer(SOCKET sock, sockaddr_storage& peer, int& peer_len) {
    std::lock_guard<std::mutex> g(g_pending_udp_mutex);
    auto it = g_udp_connected_peers.find(sock);
    if (it == g_udp_connected_peers.end()) return false;
    peer = it->second.first;
    peer_len = it->second.second;
    return true;
}

LPWSABUF register_pending_udp_recv(SOCKET sock,
                                   LPWSABUF buffers,
                                   DWORD count,
                                   sockaddr* from,
                                   LPINT fromlen,
                                   LPDWORD flags,
                                   LPWSAOVERLAPPED ov,
                                   LPWSAOVERLAPPED_COMPLETION_ROUTINE completion,
                                   bool completion_wrapped) {
    SocketEntry entry;
    if (!ov || !buffers || count != 1 || !buffers->buf || buffers->len == 0 ||
        !g_socket_manager.get(sock, entry) || !entry.is_udp) {
        return nullptr;
    }

    auto pending = std::make_shared<PendingUdpRecv>();
    pending->sock = sock;
    pending->app_buffer = buffers->buf;
    pending->app_capacity = buffers->len;
    pending->from = from;
    pending->fromlen = fromlen;
    pending->flags = flags;
    pending->completion = completion;
    pending->completion_wrapped = completion_wrapped;

    std::lock_guard<std::mutex> g(g_pending_udp_mutex);
    g_pending_udp_recvs[ov] = pending;
    return buffers;
}

LPWSAMSG register_pending_udp_recv_msg(SOCKET sock, LPWSAMSG message,
                                       LPWSAOVERLAPPED ov,
                                       LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) {
    SocketEntry entry;
    if (!ov || !message || !message->lpBuffers || message->dwBufferCount != 1 ||
        !message->lpBuffers->buf || !g_socket_manager.get(sock, entry) || !entry.is_udp) {
        return nullptr;
    }

    auto pending = std::make_shared<PendingUdpRecv>();
    pending->sock = sock;
    pending->app_buffer = message->lpBuffers->buf;
    pending->app_capacity = message->lpBuffers->len;
    pending->completion = completion;
    pending->completion_wrapped = completion != nullptr;
    pending->app_message = message;
    pending->relay_message = *message;
    pending->from = pending->relay_message.name;
    pending->fromlen = &pending->relay_message.namelen;
    pending->flags = &pending->relay_message.dwFlags;

    auto* relay_message = &pending->relay_message;
    std::lock_guard<std::mutex> g(g_pending_udp_mutex);
    g_pending_udp_recvs[ov] = pending;
    return relay_message;
}

void erase_pending_udp_recv(LPWSAOVERLAPPED ov) {
    if (!ov) return;
    std::lock_guard<std::mutex> g(g_pending_udp_mutex);
    g_pending_udp_recvs.erase(ov);
}

DWORD finish_pending_udp_send(LPWSAOVERLAPPED ov, DWORD bytes) {
    std::lock_guard<std::mutex> g(g_pending_udp_mutex);
    auto it = g_pending_udp_sends.find(ov);
    if (it == g_pending_udp_sends.end()) return bytes;
    DWORD app_bytes = it->second->app_bytes;
    g_pending_udp_sends.erase(it);
    return app_bytes;
}

void erase_pending_udp_send(LPWSAOVERLAPPED ov) {
    if (!ov) return;
    std::lock_guard<std::mutex> g(g_pending_udp_mutex);
    g_pending_udp_sends.erase(ov);
}

bool has_pending_udp_receive(LPWSAOVERLAPPED ov) {
    std::lock_guard<std::mutex> g(g_pending_udp_mutex);
    return g_pending_udp_recvs.contains(ov);
}

void CALLBACK MyWsaSendToCompletion(DWORD error, DWORD bytes,
                                    LPWSAOVERLAPPED ov, DWORD flags) {
    LPWSAOVERLAPPED_COMPLETION_ROUTINE completion = nullptr;
    DWORD app_bytes = bytes;
    {
        std::lock_guard<std::mutex> g(g_pending_udp_mutex);
        auto it = g_pending_udp_sends.find(ov);
        if (it != g_pending_udp_sends.end()) {
            completion = it->second->completion;
            app_bytes = it->second->app_bytes;
            g_pending_udp_sends.erase(it);
        }
    }
    if (completion) completion(error, error == 0 ? app_bytes : bytes, ov, flags);
}

DWORD unwrap_pending_udp_recv(LPWSAOVERLAPPED ov, DWORD bytes, bool complete) {
    std::lock_guard<std::mutex> g(g_pending_udp_mutex);
    auto it = g_pending_udp_recvs.find(ov);
    if (it == g_pending_udp_recvs.end()) {
        return bytes;
    }

    auto& pending = *it->second;
    if (pending.finished) {
        return pending.final_bytes;
    }
    if (!pending.app_buffer || bytes == 0) {
        if (bytes > 0 && pending.from && pending.fromlen) {
            rewrite_source_from_bridge(pending.sock, pending.from, pending.fromlen);
        }
        pending.finished = complete;
        pending.final_bytes = bytes;
        return bytes;
    }

    auto copied_bytes = (bytes < pending.app_capacity) ? bytes : pending.app_capacity;
    if (pending.from && pending.fromlen) {
        rewrite_source_from_bridge(pending.sock, pending.from, pending.fromlen);
    }
    if (pending.app_message) {
        pending.app_message->namelen = pending.fromlen ? *pending.fromlen : pending.relay_message.namelen;
        pending.app_message->dwFlags = pending.relay_message.dwFlags;
    }
    pending.finished = complete;
    pending.final_bytes = copied_bytes;
    return pending.final_bytes;
}

void CALLBACK MyWsaRecvFromCompletion(DWORD error, DWORD bytes, LPWSAOVERLAPPED ov, DWORD flags) {
    LPWSAOVERLAPPED_COMPLETION_ROUTINE completion = nullptr;
    {
        std::lock_guard<std::mutex> g(g_pending_udp_mutex);
        auto it = g_pending_udp_recvs.find(ov);
        if (it != g_pending_udp_recvs.end()) {
            completion = it->second->completion;
        }
    }

    DWORD final_bytes = bytes;
    if (error == 0 && bytes > 0) {
        final_bytes = unwrap_pending_udp_recv(ov, bytes, true);
    }
    erase_pending_udp_recv(ov);

    if (completion) {
        completion(error, final_bytes, ov, flags);
    }
}

std::wstring extract_image_path(LPCWSTR app, LPCWSTR cmd) {
    if (app && *app) {
        return app;
    }
    if (!cmd || !*cmd) {
        return {};
    }

    std::wstring_view line(cmd);
    if (line.front() == L'"') {
        auto end = line.find(L'"', 1);
        return end == std::wstring_view::npos
            ? std::wstring(line.substr(1))
            : std::wstring(line.substr(1, end - 1));
    }

    auto end = line.find_first_of(L" \t");
    return end == std::wstring_view::npos
        ? std::wstring(line)
        : std::wstring(line.substr(0, end));
}

bool is_embedded_transport_destination(const sockaddr* to) {
    if (!to || !g_options.tunnel.enabled || to->sa_family != AF_INET) return false;
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(to);
    return ntohs(ipv4->sin_port) == g_options.tunnel.port;
}

bool is_loopback_destination(const sockaddr* to) {
    if (!to) return false;
    if (to->sa_family == AF_INET) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(to);
        return (ntohl(ipv4->sin_addr.s_addr) >> 24) == 127;
    }
    if (to->sa_family == AF_INET6) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(to);
        return IN6_IS_ADDR_LOOPBACK(&ipv6->sin6_addr) != 0;
    }
    return false;
}

// ---- Hook implementations --------------------------------------------------

SOCKET WSAAPI MySocket(int af, int type, int protocol) {
    SOCKET s = real_socket(af, type, protocol);
    if (s != INVALID_SOCKET && !is_embedded_runtime_thread()) {
        g_socket_manager.add(s, type, protocol);
    }
    return s;
}

SOCKET WSAAPI MyWSASocket(int af, int type, int protocol, LPWSAPROTOCOL_INFOW info,
                          GROUP g, DWORD flags) {
    SOCKET s = real_wsa_socket(af, type, protocol, info, g, flags);
    if (s != INVALID_SOCKET && !is_embedded_runtime_thread()) {
        g_socket_manager.add(s, type, protocol);
    }
    return s;
}

int WSAAPI MyConnect(SOCKET s, const sockaddr* name, int name_len) {
    SocketEntry entry;
    if (!g_socket_manager.get(s, entry) || !name ||
        name_len < static_cast<int>(sizeof(name->sa_family))) {
        return real_connect(s, name, name_len);
    }

    if (entry.is_udp && name->sa_family == AF_UNSPEC) {
        std::lock_guard<std::mutex> g(g_pending_udp_mutex);
        g_udp_connected_peers.erase(s);
        return 0;
    }

    if ((name->sa_family != AF_INET && name->sa_family != AF_INET6) ||
        name_len > static_cast<int>(sizeof(sockaddr_storage)) ||
        is_loopback_destination(name) || is_embedded_transport_destination(name)) {
        return real_connect(s, name, name_len);
    }

    if (!bridge_ready()) {
        WSASetLastError(WSAECONNREFUSED);
        return SOCKET_ERROR;
    }

    if (entry.is_tcp) {
        BridgeBinding bridge;
        if (!ensure_bridge_binding(s, name, name_len, false, bridge)) {
            WSASetLastError(WSAECONNREFUSED);
            return SOCKET_ERROR;
        }
        return real_connect(
            s,
            reinterpret_cast<const sockaddr*>(&bridge.loopback),
            static_cast<int>(sizeof(bridge.loopback)));
    }

    if (entry.is_udp) {
        sockaddr_storage peer{};
        std::memcpy(&peer, name, static_cast<std::size_t>(name_len));
        {
            std::lock_guard<std::mutex> g(g_pending_udp_mutex);
            g_udp_connected_peers[s] = {peer, name_len};
        }
        BridgeBinding bridge;
        if (!ensure_bridge_binding(s, name, name_len, true, bridge)) {
            WSASetLastError(WSAECONNREFUSED);
            return SOCKET_ERROR;
        }
        return 0;
    }

    return real_connect(s, name, name_len);
}

int WSAAPI MyGetpeername(SOCKET s, sockaddr* name, int* name_len) {
    BridgeBinding binding;
    if (get_bridge_binding(s, binding) && !binding.udp) {
        if (!name || !name_len || *name_len < binding.original_peer_len) {
            WSASetLastError(WSAEFAULT);
            return SOCKET_ERROR;
        }
        std::memcpy(name, &binding.original_peer, static_cast<std::size_t>(binding.original_peer_len));
        *name_len = binding.original_peer_len;
        return 0;
    }
    sockaddr_storage peer{};
    int peer_len = 0;
    if (!get_udp_connected_peer(s, peer, peer_len)) {
        return real_getpeername(s, name, name_len);
    }
    if (!name || !name_len || *name_len < peer_len) {
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }
    std::memcpy(name, &peer, static_cast<std::size_t>(peer_len));
    *name_len = peer_len;
    return 0;
}

int WSAAPI MyWSASend(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD sent,
                     DWORD flags, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr) {
    SocketEntry entry;
    if (g_socket_manager.get(s, entry) && entry.is_udp &&
        g_udp_api_logs.fetch_add(1, std::memory_order_relaxed) < 12) {
        LOG_INFO("[udp-diag] WSASend sock={} count={} overlapped={} completion={}",
                 static_cast<unsigned long long>(s), count, ov != nullptr, cr != nullptr);
    }
    sockaddr_storage peer{};
    int peer_len = 0;
    if (bufs && count == 1 && bufs->len > 0 && entry.is_udp &&
        get_udp_connected_peer(s, peer, peer_len)) {
        BridgeBinding bridge;
        if (!bridge_ready()) {
            WSASetLastError(WSAECONNREFUSED);
            return SOCKET_ERROR;
        }
        if (ensure_bridge_binding(s, reinterpret_cast<const sockaddr*>(&peer), peer_len, true, bridge)) {
            return real_wsa_sendto(
                s,
                bufs,
                count,
                sent,
                flags,
                reinterpret_cast<const sockaddr*>(&bridge.loopback),
                sizeof(bridge.loopback),
                ov,
                cr);
        }
        WSASetLastError(WSAECONNREFUSED);
        return SOCKET_ERROR;
    }
    return real_wsa_send(s, bufs, count, sent, flags, ov, cr);
}

int WSAAPI MyWSASendTo(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD sent, DWORD flags,
                       const sockaddr* to, int to_len, LPWSAOVERLAPPED ov,
                       LPWSAOVERLAPPED_COMPLETION_ROUTINE cr) {
    if (is_loopback_destination(to) || is_embedded_transport_destination(to)) {
        return real_wsa_sendto(s, bufs, count, sent, flags, to, to_len, ov, cr);
    }

    SocketEntry entry;
    if (g_socket_manager.get(s, entry) && entry.is_udp &&
        g_udp_api_logs.fetch_add(1, std::memory_order_relaxed) < 12) {
        LOG_INFO("[udp-diag] WSASendTo sock={} count={} overlapped={} completion={}",
                 static_cast<unsigned long long>(s), count, ov != nullptr, cr != nullptr);
    }

    if (bufs && count == 1 && bufs->len > 0 && entry.is_udp) {
        if (!bridge_ready()) {
            WSASetLastError(WSAECONNREFUSED);
            return SOCKET_ERROR;
        }
        BridgeBinding bridge;
        if (ensure_bridge_binding(s, to, to_len, true, bridge)) {
            return real_wsa_sendto(
                s,
                bufs,
                count,
                sent,
                flags,
                reinterpret_cast<const sockaddr*>(&bridge.loopback),
                sizeof(bridge.loopback),
                ov,
                cr);
        }
        WSASetLastError(WSAECONNREFUSED);
        return SOCKET_ERROR;
    }
    return real_wsa_sendto(s, bufs, count, sent, flags, to, to_len, ov, cr);
}

int WSAAPI MySendto(SOCKET s, const char* buf, int len, int flags,
                    const sockaddr* to, int to_len) {
    if (is_loopback_destination(to) || is_embedded_transport_destination(to)) {
        return real_sendto(s, buf, len, flags, to, to_len);
    }

    SocketEntry entry;
    if (g_socket_manager.get(s, entry) && entry.is_udp &&
        g_udp_api_logs.fetch_add(1, std::memory_order_relaxed) < 12) {
        LOG_INFO("[udp-diag] sendto sock={} bytes={}",
                 static_cast<unsigned long long>(s), len);
    }
    if (buf && len > 0 && entry.is_udp) {
        if (!bridge_ready()) {
            WSASetLastError(WSAECONNREFUSED);
            return SOCKET_ERROR;
        }
        BridgeBinding bridge;
        if (ensure_bridge_binding(s, to, to_len, true, bridge)) {
            return real_sendto(
                s,
                buf,
                len,
                flags,
                reinterpret_cast<const sockaddr*>(&bridge.loopback),
                sizeof(bridge.loopback));
        }
        WSASetLastError(WSAECONNREFUSED);
        return SOCKET_ERROR;
    }
    return real_sendto(s, buf, len, flags, to, to_len);
}

int WSAAPI MySend(SOCKET s, const char* buf, int len, int flags) {
    SocketEntry entry;
    if (g_socket_manager.get(s, entry) && entry.is_udp &&
        g_udp_api_logs.fetch_add(1, std::memory_order_relaxed) < 12) {
        LOG_INFO("[udp-diag] send sock={} bytes={}",
                 static_cast<unsigned long long>(s), len);
    }
    sockaddr_storage peer{};
    int peer_len = 0;
    if (buf && len > 0 && entry.is_udp && get_udp_connected_peer(s, peer, peer_len)) {
        BridgeBinding bridge;
        if (!bridge_ready()) {
            WSASetLastError(WSAECONNREFUSED);
            return SOCKET_ERROR;
        }
        if (ensure_bridge_binding(s, reinterpret_cast<const sockaddr*>(&peer), peer_len, true, bridge)) {
            return real_sendto(
                s,
                buf,
                len,
                flags,
                reinterpret_cast<const sockaddr*>(&bridge.loopback),
                sizeof(bridge.loopback));
        }
        WSASetLastError(WSAECONNREFUSED);
        return SOCKET_ERROR;
    }
    return real_send(s, buf, len, flags);
}

int WSAAPI MyRecv(SOCKET s, char* buf, int len, int flags) {
    return real_recv(s, buf, len, flags);
}

int WSAAPI MyWSARecv(SOCKET s, LPWSABUF buffers, DWORD count, LPDWORD recvd,
                     LPDWORD flags, LPWSAOVERLAPPED ov,
                     LPWSAOVERLAPPED_COMPLETION_ROUTINE cr) {
    auto* async_buffer = register_pending_udp_recv(
        s, buffers, count, nullptr, nullptr, flags, ov, cr, cr != nullptr);
    auto completion = (async_buffer && cr) ? &MyWsaRecvFromCompletion : cr;
    int rc = real_wsa_recv(s, async_buffer ? async_buffer : buffers, count, recvd, flags, ov, completion);
    int receive_error = rc == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (async_buffer &&
        g_udp_receive_logs.fetch_add(1, std::memory_order_relaxed) < 24) {
        LOG_INFO("[udp-recv] WSARecv sock={} rc={} error={} overlapped={} capacity={}",
                 static_cast<unsigned long long>(s), rc, receive_error, ov != nullptr,
                 buffers->len);
    }

    if (ov && rc == 0 && recvd && *recvd > 0 && async_buffer) {
        erase_pending_udp_recv(ov);
    } else if (ov && rc == SOCKET_ERROR && receive_error != WSA_IO_PENDING) {
        erase_pending_udp_recv(ov);
    }
    if (rc == SOCKET_ERROR) WSASetLastError(receive_error);
    return rc;
}

int WSAAPI MyClosesocket(SOCKET s) {
    // Winsock may still complete cancelled overlapped operations after
    // closesocket returns. Their backing buffers must remain alive until the
    // completion hook consumes them.
    close_bridge_binding(s);
    {
        std::lock_guard<std::mutex> g(g_pending_udp_mutex);
        g_udp_connected_peers.erase(s);
    }
    g_socket_manager.remove(s);
    return real_closesocket(s);
}

// recvfrom hook - strips the SOCKS5 UDP header from inbound packets so
// Discord sees plain UDP voice data with the original source address
// rather than the relay's address.
int WSAAPI MyRecvfrom(SOCKET s, char* buf, int len, int flags,
                      sockaddr* from, int* fromlen) {
    int n = real_recvfrom(s, buf, len, flags, from, fromlen);
    int receive_error = n == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (n > 0) {
        rewrite_source_from_bridge(s, from, fromlen);
    }
    if (n == SOCKET_ERROR) WSASetLastError(receive_error);
    return n;
}

// Receive into an expanded relay buffer so the SOCKS5 header cannot turn a
// valid voice datagram into WSAEMSGSIZE before we strip the header.
int WSAAPI MyWSARecvFrom(SOCKET s, LPWSABUF buffers, DWORD count, LPDWORD recvd,
                         LPDWORD flags, sockaddr* from, LPINT fromlen,
                         LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr) {
    auto* async_buffer = register_pending_udp_recv(
        s, buffers, count, from, fromlen, flags, ov, cr, cr != nullptr);

    auto completion = (async_buffer && cr) ? &MyWsaRecvFromCompletion : cr;
    int rc = real_wsa_recvfrom(s, async_buffer ? async_buffer : buffers, count, recvd, flags, from, fromlen, ov, completion);
    int receive_error = rc == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (async_buffer &&
        g_udp_receive_logs.fetch_add(1, std::memory_order_relaxed) < 24) {
        LOG_INFO("[udp-recv] WSARecvFrom sock={} rc={} error={} overlapped={} capacity={}",
                 static_cast<unsigned long long>(s), rc, receive_error, ov != nullptr,
                 buffers->len);
    }

    if (!ov && rc == 0 && recvd && *recvd > 0) {
        rewrite_source_from_bridge(s, from, fromlen);
    } else if (ov && rc == 0 && recvd && *recvd > 0 && async_buffer) {
        erase_pending_udp_recv(ov);
    } else if (ov && rc == SOCKET_ERROR && receive_error != WSA_IO_PENDING) {
        erase_pending_udp_recv(ov);
    }
    if (rc == SOCKET_ERROR) WSASetLastError(receive_error);
    return rc;
}

INT PASCAL MyWSARecvMsg(SOCKET s, LPWSAMSG message, LPDWORD recvd,
                        LPWSAOVERLAPPED ov,
                        LPWSAOVERLAPPED_COMPLETION_ROUTINE cr) {
    if (!real_wsa_recv_msg) {
        WSASetLastError(WSAEOPNOTSUPP);
        return SOCKET_ERROR;
    }

    auto* async_message = register_pending_udp_recv_msg(s, message, ov, cr);

    auto completion = (async_message && cr) ? &MyWsaRecvFromCompletion : cr;
    int rc = real_wsa_recv_msg(s, async_message ? async_message : message, recvd, ov, completion);
    int receive_error = rc == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (async_message &&
        g_udp_receive_logs.fetch_add(1, std::memory_order_relaxed) < 24) {
        LOG_INFO("[udp-recv] WSARecvMsg sock={} rc={} error={} overlapped={} capacity={}",
                 static_cast<unsigned long long>(s), rc, receive_error, ov != nullptr,
                 message->lpBuffers->len);
    }

    if (!ov && rc == 0 && recvd && *recvd > 0 && message) {
        rewrite_source_from_bridge(s, message->name, &message->namelen);
    } else if (ov && rc == 0 && recvd && *recvd > 0 && async_message) {
        erase_pending_udp_recv(ov);
    } else if (ov && rc == SOCKET_ERROR && receive_error != WSA_IO_PENDING) {
        erase_pending_udp_recv(ov);
    }
    if (rc == SOCKET_ERROR) WSASetLastError(receive_error);
    return rc;
}

int WSAAPI MyWSAIoctl(SOCKET s, DWORD code, LPVOID in_buffer, DWORD in_size,
                      LPVOID out_buffer, DWORD out_size, LPDWORD bytes,
                      LPWSAOVERLAPPED ov,
                      LPWSAOVERLAPPED_COMPLETION_ROUTINE cr) {
    int rc = real_wsa_ioctl(s, code, in_buffer, in_size, out_buffer, out_size,
                            bytes, ov, cr);
    int error = rc == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (rc == 0 && code == SIO_GET_EXTENSION_FUNCTION_POINTER &&
        in_buffer && in_size >= sizeof(GUID) && out_buffer &&
        out_size >= sizeof(LPFN_WSARECVMSG) &&
        IsEqualGUID(*static_cast<GUID*>(in_buffer), WSAID_WSARECVMSG)) {
        auto* function = static_cast<LPFN_WSARECVMSG*>(out_buffer);
        real_wsa_recv_msg = reinterpret_cast<WSARecvMsg_t>(*function);
        *function = reinterpret_cast<LPFN_WSARECVMSG>(&MyWSARecvMsg);
        if (bytes) *bytes = sizeof(LPFN_WSARECVMSG);
    }
    if (rc == SOCKET_ERROR) WSASetLastError(error);
    return rc;
}

BOOL WSAAPI MyWSAGetOverlappedResult(SOCKET s, LPWSAOVERLAPPED ov, LPDWORD bytes,
                                     BOOL wait, LPDWORD flags) {
    BOOL ok = real_wsa_get_overlapped_result(s, ov, bytes, wait, flags);
    if (!ov) {
        return ok;
    }
    if (!ok) {
        DWORD err = WSAGetLastError();
        if (err != WSA_IO_INCOMPLETE) {
            erase_pending_udp_recv(ov);
            erase_pending_udp_send(ov);
        }
        return ok;
    }
    if (bytes) {
        *bytes = finish_pending_udp_send(ov, *bytes);
        erase_pending_udp_recv(ov);
    }
    return ok;
}

BOOL WINAPI MyGetQueuedCompletionStatus(HANDLE port, LPDWORD bytes, PULONG_PTR key,
                                         LPOVERLAPPED* ov, DWORD ms) {
    BOOL ok = real_get_queued_completion_status(port, bytes, key, ov, ms);
    DWORD error = ok ? 0 : GetLastError();
    if (ov && *ov && bytes && ok) {
        auto* wsa_ov = reinterpret_cast<LPWSAOVERLAPPED>(*ov);
        *bytes = finish_pending_udp_send(wsa_ov, *bytes);
        erase_pending_udp_recv(wsa_ov);
    } else if (ov && *ov && !ok) {
        auto* wsa_ov = reinterpret_cast<LPWSAOVERLAPPED>(*ov);
        if (g_udp_completion_logs.fetch_add(1, std::memory_order_relaxed) < 16) {
            LOG_INFO("[udp-diag] IOCP completion failed error={} bytes={}",
                     error, bytes ? *bytes : 0);
        }
        erase_pending_udp_recv(wsa_ov);
        erase_pending_udp_send(wsa_ov);
    }
    if (!ok) SetLastError(error);
    return ok;
}

BOOL WINAPI MyGetQueuedCompletionStatusEx(HANDLE port, LPOVERLAPPED_ENTRY entries,
                                          ULONG count, PULONG removed, DWORD ms,
                                          BOOL alertable) {
    BOOL ok = real_get_queued_completion_status_ex(port, entries, count, removed, ms, alertable);
    if (!ok || !entries || !removed) {
        return ok;
    }
    for (ULONG i = 0; i < *removed; ++i) {
        if (entries[i].lpOverlapped) {
            auto* wsa_ov = reinterpret_cast<LPWSAOVERLAPPED>(entries[i].lpOverlapped);
            entries[i].dwNumberOfBytesTransferred = finish_pending_udp_send(
                wsa_ov,
                entries[i].dwNumberOfBytesTransferred);
            erase_pending_udp_recv(wsa_ov);
        }
    }
    return ok;
}

BOOL WINAPI MySetFileCompletionNotificationModes(HANDLE handle, UCHAR flags) {
    SocketEntry entry;
    auto socket = reinterpret_cast<SOCKET>(handle);
    if (g_socket_manager.get(socket, entry) && entry.is_udp &&
        (flags & FILE_SKIP_COMPLETION_PORT_ON_SUCCESS) != 0) {
        // libuv treats this error as a supported fallback and keeps using the
        // exported WSARecv/WSARecvFrom functions that we can unwrap safely.
        if (g_libuv_receive_logs.fetch_add(1, std::memory_order_relaxed) < 24) {
            LOG_INFO("[libuv] disabled native receive bypass for UDP sock={}",
                     static_cast<unsigned long long>(socket));
        }
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    return real_set_file_completion_notification_modes(handle, flags);
}

LPWSTR WINAPI MyGetCommandLineW() {
    return const_cast<LPWSTR>(g_cmdline_cache.c_str());
}

DWORD WINAPI MyGetEnvironmentVariableW(LPCWSTR name, LPWSTR buf, DWORD size) {
    if (name && g_options.proxy.is_http()) {
        if (_wcsicmp(name, L"http_proxy") == 0 ||
            _wcsicmp(name, L"https_proxy") == 0 ||
            _wcsicmp(name, L"HTTP_PROXY") == 0 ||
            _wcsicmp(name, L"HTTPS_PROXY") == 0) {
            auto value = widen_utf8(g_options.proxy.format_http_env());
            return write_env_override(value, buf, size);
        }
    }
    return real_get_env(name, buf, size);
}

BOOL WINAPI MyCreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa,
                             LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags,
                             LPVOID env, LPCWSTR cwd, LPSTARTUPINFOW si,
                             LPPROCESS_INFORMATION pi) {
    auto image = extract_image_path(app, cmd);
    if (!image.empty()) {
        std::filesystem::path name(image);
        auto leaf = name.filename().wstring();
        if (discord_dirs::is_discord_executable_w(leaf) ||
            _wcsicmp(leaf.c_str(), L"reg.exe") == 0) {
            discord_dirs::copy_dll_into_all_app_dirs();
        }
    }
    return real_create_process(app, cmd, pa, ta, inherit, flags, env, cwd, si, pi);
}

template <typename T>
void create_hook(LPCSTR module, LPCSTR name, void* detour, T*& real) {
    HMODULE m = GetModuleHandleA(module);
    if (!m) m = LoadLibraryA(module);
    if (!m) return;
    auto target = GetProcAddress(m, name);
    if (!target) return;
    if (MH_CreateHook(target, detour, reinterpret_cast<LPVOID*>(&real)) == MH_OK) {
        MH_EnableHook(target);
    }
}

} // namespace

void build_command_line_cache() {
    LPWSTR original = GetCommandLineW();
    g_cmdline_cache = original ? original : L"";
    if (!g_options.proxy.is_http()) {
        return;
    }

    std::wstring proxy_flag = L" --proxy-server=" + widen_utf8(g_options.proxy.format_chrome_proxy());
    if (proxy_flag.size() <= std::wstring_view(L" --proxy-server=").size()) {
        return;
    }
    if (g_cmdline_cache.find(L"--proxy-server=") != std::wstring::npos) {
        return;
    }
    g_cmdline_cache += proxy_flag;
}

void install() {
    if (MH_Initialize() != MH_OK) {
        LOG_ERROR("MinHook init failed");
        return;
    }

    create_hook("ws2_32.dll", "socket",      reinterpret_cast<void*>(&MySocket),      real_socket);
    create_hook("ws2_32.dll", "WSASocketW",  reinterpret_cast<void*>(&MyWSASocket),   real_wsa_socket);
    create_hook("ws2_32.dll", "connect",     reinterpret_cast<void*>(&MyConnect),     real_connect);
    create_hook("ws2_32.dll", "getpeername", reinterpret_cast<void*>(&MyGetpeername), real_getpeername);
    create_hook("ws2_32.dll", "WSASend",     reinterpret_cast<void*>(&MyWSASend),     real_wsa_send);
    create_hook("ws2_32.dll", "WSASendTo",   reinterpret_cast<void*>(&MyWSASendTo),   real_wsa_sendto);
    create_hook("ws2_32.dll", "sendto",      reinterpret_cast<void*>(&MySendto),      real_sendto);
    create_hook("ws2_32.dll", "send",        reinterpret_cast<void*>(&MySend),        real_send);
    create_hook("ws2_32.dll", "recv",        reinterpret_cast<void*>(&MyRecv),        real_recv);
    create_hook("ws2_32.dll", "WSARecv",     reinterpret_cast<void*>(&MyWSARecv),     real_wsa_recv);
    create_hook("ws2_32.dll", "closesocket", reinterpret_cast<void*>(&MyClosesocket), real_closesocket);
    create_hook("ws2_32.dll", "recvfrom",    reinterpret_cast<void*>(&MyRecvfrom),    real_recvfrom);
    create_hook("ws2_32.dll", "WSARecvFrom", reinterpret_cast<void*>(&MyWSARecvFrom), real_wsa_recvfrom);
    create_hook("ws2_32.dll", "WSAIoctl",    reinterpret_cast<void*>(&MyWSAIoctl),    real_wsa_ioctl);
    create_hook("ws2_32.dll", "WSAGetOverlappedResult",
                reinterpret_cast<void*>(&MyWSAGetOverlappedResult),
                real_wsa_get_overlapped_result);
    // MinHook stores the sendto trampoline in real_sendto, so relay traffic
    // bypasses our detour instead of being wrapped recursively.
    real_io::real_sendto_win = real_sendto;
    create_hook("kernel32.dll", "GetCommandLineW",        reinterpret_cast<void*>(&MyGetCommandLineW),        real_get_cmdline);
    create_hook("kernel32.dll", "GetEnvironmentVariableW", reinterpret_cast<void*>(&MyGetEnvironmentVariableW), real_get_env);
    create_hook("kernel32.dll", "GetQueuedCompletionStatus",
                reinterpret_cast<void*>(&MyGetQueuedCompletionStatus),
                real_get_queued_completion_status);
    create_hook("kernel32.dll", "GetQueuedCompletionStatusEx",
                reinterpret_cast<void*>(&MyGetQueuedCompletionStatusEx),
                real_get_queued_completion_status_ex);
    create_hook("kernel32.dll", "SetFileCompletionNotificationModes",
                reinterpret_cast<void*>(&MySetFileCompletionNotificationModes),
                real_set_file_completion_notification_modes);
    create_hook("kernel32.dll", "CreateProcessW",          reinterpret_cast<void*>(&MyCreateProcessW),          real_create_process);

    LOG_INFO("hooks installed");
}

void uninstall() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

} // namespace discord_tunnel::hooks
