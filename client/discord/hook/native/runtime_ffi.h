#pragma once

#include <stdint.h>

extern "C" {

int dt_embedded_start(const char* server_host,
                      uint16_t server_port,
                      const char* token,
                      const char* ca_pem,
                      int insecure,
                      uint16_t listen_port);

int dt_bridge_is_ready();

int dt_bridge_tcp_open(const char* host,
                       uint16_t port,
                       uint64_t* out_bridge_id,
                       uint16_t* out_loopback_port);

int dt_bridge_udp_open(const char* host,
                       uint16_t port,
                       uint64_t* out_bridge_id,
                       uint16_t* out_loopback_port);

int dt_bridge_close(uint64_t bridge_id);

}

constexpr int DT_OK = 0;
constexpr int DT_ERR_TRANSPORT = -6;
constexpr int DT_ERR_TRANSPORT_AUTH = -7;
