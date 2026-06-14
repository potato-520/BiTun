#include "socks5.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void socks5_init(socks5_context_t *ctx) {
    ctx->state = SOCKS5_STATE_INIT;
    ctx->methods_left = 0;
    ctx->addr_type = 0;
    ctx->addr_len = 0;
    ctx->addr_idx = 0;
    ctx->port = 0;
    ctx->port_idx = 0;
    ctx->domain = NULL;
    memset(ctx->ipv4, 0, 4);
    memset(ctx->ipv6, 0, 16);
    ctx->req_hdr_idx = 0;
    ctx->method_idx = 0;
    ctx->found_no_auth = 0;
}

void socks5_free(socks5_context_t *ctx) {
    if (ctx->domain) {
        free(ctx->domain);
        ctx->domain = NULL;
    }
}

int socks5_handle_input(socks5_context_t *ctx, const uint8_t *buf, size_t len,
                        uint8_t *resp_out, size_t *resp_len) {
    *resp_len = 0;
    size_t i = 0;
    uint8_t b;

    for (i = 0; i < len; i++) {
        b = buf[i];

        switch (ctx->state) {
            case SOCKS5_STATE_INIT:
                /* SOCKS5 handshake version check */
                if (b != 0x05) {
                    ctx->state = SOCKS5_STATE_ERROR;
                    return -1;
                }
                ctx->state = SOCKS5_STATE_METHODS;
                break;

            case SOCKS5_STATE_METHODS:
                if (ctx->method_idx == 0) {
                    ctx->methods_left = b;
                    if (ctx->methods_left == 0) {
                        ctx->state = SOCKS5_STATE_ERROR;
                        return -1;
                    }
                    ctx->method_idx = 1;
                } else {
                    if (b == 0x00) {
                        ctx->found_no_auth = 1;
                    }
                    ctx->method_idx++;
                    if (ctx->method_idx > ctx->methods_left) {
                        if (ctx->found_no_auth) {
                            resp_out[0] = 0x05;
                            resp_out[1] = 0x00; // NO AUTHENTICATION REQUIRED
                            *resp_len = 2;
                            ctx->state = SOCKS5_STATE_REQ_HEADER;
                            ctx->req_hdr_idx = 0;
                        } else {
                            resp_out[0] = 0x05;
                            resp_out[1] = 0xFF; // No acceptable methods
                            *resp_len = 2;
                            ctx->state = SOCKS5_STATE_ERROR;
                            return -1;
                        }
                    }
                }
                break;

            case SOCKS5_STATE_REQ_HEADER:
                /* Accumulate 4 bytes of request header: VER, CMD, RSV, ATYP */
                ctx->req_hdr[ctx->req_hdr_idx++] = b;
                if (ctx->req_hdr_idx == 4) {
                    if (ctx->req_hdr[0] != 0x05) { // VER
                        ctx->state = SOCKS5_STATE_ERROR;
                        return -1;
                    }
                    if (ctx->req_hdr[1] != 0x01) { // CMD: must be 0x01 CONNECT
                        /* Command not supported response */
                        resp_out[0] = 0x05;
                        resp_out[1] = 0x07; // Command not supported
                        resp_out[2] = 0x00;
                        resp_out[3] = 0x01; // IPv4 address type
                        memset(&resp_out[4], 0, 6); // Bind Address & Port
                        *resp_len = 10;
                        ctx->state = SOCKS5_STATE_ERROR;
                        return -1;
                    }
                    
                    ctx->addr_type = ctx->req_hdr[3]; // ATYP
                    if (ctx->addr_type == 0x01) { // IPv4
                        ctx->addr_len = 4;
                        ctx->addr_idx = 0;
                        ctx->state = SOCKS5_STATE_REQ_ADDR_BODY;
                    } else if (ctx->addr_type == 0x03) { // Domain name
                        ctx->state = SOCKS5_STATE_REQ_ADDR_LEN;
                    } else if (ctx->addr_type == 0x04) { // IPv6
                        ctx->addr_len = 16;
                        ctx->addr_idx = 0;
                        ctx->state = SOCKS5_STATE_REQ_ADDR_BODY;
                    } else {
                        /* Address type not supported response */
                        resp_out[0] = 0x05;
                        resp_out[1] = 0x08; // Address type not supported
                        resp_out[2] = 0x00;
                        resp_out[3] = 0x01;
                        memset(&resp_out[4], 0, 6);
                        *resp_len = 10;
                        ctx->state = SOCKS5_STATE_ERROR;
                        return -1;
                    }
                }
                break;

            case SOCKS5_STATE_REQ_ADDR_LEN:
                /* b is domain length */
                ctx->addr_len = b;
                if (ctx->addr_len == 0) {
                    ctx->state = SOCKS5_STATE_ERROR;
                    return -1;
                }
                ctx->domain = (char *)malloc(ctx->addr_len + 1);
                if (!ctx->domain) {
                    ctx->state = SOCKS5_STATE_ERROR;
                    return -1;
                }
                ctx->addr_idx = 0;
                ctx->state = SOCKS5_STATE_REQ_ADDR_BODY;
                break;

            case SOCKS5_STATE_REQ_ADDR_BODY:
                if (ctx->addr_type == 0x01) {
                    ctx->ipv4[ctx->addr_idx++] = b;
                } else if (ctx->addr_type == 0x03) {
                    ctx->domain[ctx->addr_idx++] = (char)b;
                } else if (ctx->addr_type == 0x04) {
                    ctx->ipv6[ctx->addr_idx++] = b;
                }

                if (ctx->addr_idx == ctx->addr_len) {
                    if (ctx->addr_type == 0x03) {
                        ctx->domain[ctx->addr_len] = '\0';
                    }
                    ctx->port = 0;
                    ctx->port_idx = 0;
                    ctx->state = SOCKS5_STATE_REQ_PORT;
                }
                break;

            case SOCKS5_STATE_REQ_PORT:
                ctx->port = (ctx->port << 8) | b;
                ctx->port_idx++;
                if (ctx->port_idx == 2) {
                    ctx->state = SOCKS5_STATE_CONNECTING;
                    /* Address resolved successfully! Return 1 to indicate we need to trigger connection */
                    return 1;
                }
                break;

            case SOCKS5_STATE_CONNECTING:
            case SOCKS5_STATE_FORWARDING:
                /* Already resolved or forwarding, ignore subsequent handshake parses */
                return 0;

            default:
                return -1;
        }
    }
    return 0;
}
