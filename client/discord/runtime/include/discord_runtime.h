#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DT_OK = 0,
    DT_EVENT_EMPTY = 1,
    DT_ERR_QUEUE_FULL = -1,
    DT_ERR_DISCONNECTED = -2,
    DT_ERR_STALE_EPOCH = -3,
    DT_ERR_PANIC = -4,
    DT_ERR_INVALID = -5,
    DT_ERR_TRANSPORT = -6,
    DT_ERR_TRANSPORT_AUTH = -7,
};

int dt_embedded_start(
    const char* server_host,
    uint16_t server_port,
    const char* token,
    const char* ca_pem,
    int insecure,
    uint16_t listen_port);

int dt_bridge_is_ready(void);

int dt_bridge_tcp_open(
    const char* host,
    uint16_t port,
    uint64_t* out_bridge_id,
    uint16_t* out_loopback_port);

int dt_bridge_udp_open(
    const char* host,
    uint16_t port,
    uint64_t* out_bridge_id,
    uint16_t* out_loopback_port);

int dt_bridge_close(uint64_t bridge_id);

#ifdef __cplusplus
}
#endif
