#ifndef BITUN_SOCKS5_H
#define BITUN_SOCKS5_H

#include <stdint.h>
#include <stddef.h>

/* SOCKS5 States */
typedef enum {
    SOCKS5_STATE_INIT = 0,
    SOCKS5_STATE_METHODS,
    SOCKS5_STATE_REQ_HEADER,
    SOCKS5_STATE_REQ_ADDR_LEN,
    SOCKS5_STATE_REQ_ADDR_BODY,
    SOCKS5_STATE_REQ_PORT,
    SOCKS5_STATE_CONNECTING,
    SOCKS5_STATE_FORWARDING,
    SOCKS5_STATE_ERROR
} socks5_state_t;

/* SOCKS5 Stateless parsing context */
typedef struct {
    socks5_state_t state;
    uint8_t methods_left;
    uint8_t addr_type;
    uint16_t addr_len;
    uint16_t addr_idx;
    uint16_t port;
    uint8_t port_idx;
    char *domain;             // Dynamic domain buffer (freed after resolve)
    uint8_t ipv4[4];
    uint8_t ipv6[16];
    uint8_t req_hdr[4];
    uint8_t req_hdr_idx;
    uint8_t method_idx;
    int found_no_auth;
} socks5_context_t;

void socks5_init(socks5_context_t *ctx);
void socks5_free(socks5_context_t *ctx);

/* 
 * Stream parser: processes incoming SOCKS5 handshake bytes and writes response bytes.
 * Returns:
 *   1  -> Destination target address and port resolved! Need to connect.
 *   0  -> Normal processing, state machine is waiting for more bytes.
 *  -1  -> Protocol/parsing error.
 */
int socks5_handle_input(socks5_context_t *ctx, const uint8_t *buf, size_t len,
                        uint8_t *resp_out, size_t *resp_len);

#endif /* BITUN_SOCKS5_H */
