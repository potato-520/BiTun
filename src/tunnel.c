#define _GNU_SOURCE
#include "tunnel.h"
#include "bitun_osal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <signal.h>

extern volatile sig_atomic_t g_should_exit;

#define MAX_EVENTS 1024
#define DEFAULT_WND_SIZE 4096  /* 4KB Channel Window */
#define BACKPRESSURE_THRES 32  /* 32 KCP Packets */

static int const_memcmp(const void *a, const void *b, size_t len) {
    const unsigned char *ua = a;
    const unsigned char *ub = b;
    unsigned char diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (ua[i] ^ ub[i]);
    }
    return diff != 0;
}

/* KCP Output Callback - Encrypts and sends over UDP */
static int kcp_output_cb(const char *buf, int len, ikcpcb *kcp, void *user) {
    (void)kcp;
    tunnel_t *tun = (tunnel_t *)user;
    if (!tun->peer_addr_valid) return 0;

    uint64_t seq = ++tun->seq_send;
    uint8_t nonce[AEAD_NONCE_LEN];
    uint64_t seq_be = bitun_htobe64(seq);
    memcpy(nonce, &seq_be, 8);
    memset(nonce + 8, 0, 4);

    /* Packet layout: [Seq (8B)] [Nonce (12B)] [Tag (16B)] [Ciphertext (len B)] */
    int out_len = 8 + AEAD_NONCE_LEN + AEAD_TAG_LEN + len;
    uint8_t *packet = malloc(out_len);
    if (!packet) return -1;

    uint8_t tag[AEAD_TAG_LEN];
    int ciphertext_len = encrypt_chacha20_poly1305(tun->session_key, nonce,
                                                   (const uint8_t *)buf, len,
                                                   packet + 8 + AEAD_NONCE_LEN + AEAD_TAG_LEN,
                                                   tag);
    if (ciphertext_len < 0) {
        free(packet);
        return -1;
    }

    memcpy(packet, &seq_be, 8);
    memcpy(packet + 8, nonce, AEAD_NONCE_LEN);
    memcpy(packet + 8 + AEAD_NONCE_LEN, tag, AEAD_TAG_LEN);

    bitun_osal_socket_sendto(tun->udp_fd, packet, out_len, 0,
                             (struct sockaddr *)&tun->peer_addr, sizeof(tun->peer_addr));
    free(packet);
    return 0;
}

/* Initialize Tunnel */
int tunnel_init(tunnel_t *tun, const tunnel_config_t *config) {
    memset(tun, 0, sizeof(tunnel_t));
    tun->config = *config;
    tun->state = STATE_DISCONNECTED;
    anti_replay_init(&tun->anti_replay_win);
    bitun_osal_mutex_create(&tun->mutex);
    
    /* Open UDP socket */
    tun->udp_fd = bitun_osal_socket_create(AF_INET, SOCK_DGRAM, 0);
    if (tun->udp_fd < 0) return -1;
    bitun_osal_socket_set_nonblocking(tun->udp_fd);

    memset(&tun->local_addr, 0, sizeof(tun->local_addr));
    tun->local_addr.sin_family = AF_INET;
    tun->local_addr.sin_port = htons(config->local_port);
    if (config->local_ip) {
        inet_pton(AF_INET, config->local_ip, &tun->local_addr.sin_addr);
    } else {
        tun->local_addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bitun_osal_socket_bind(tun->udp_fd, (struct sockaddr *)&tun->local_addr, sizeof(tun->local_addr)) < 0) {
        bitun_osal_socket_close(tun->udp_fd);
        return -1;
    }

    /* Configure Peer Address if active */
    if (config->remote_ip && strcmp(config->remote_ip, "0.0.0.0") != 0 && config->remote_port > 0) {
        memset(&tun->peer_addr, 0, sizeof(tun->peer_addr));
        tun->peer_addr.sin_family = AF_INET;
        tun->peer_addr.sin_port = htons(config->remote_port);
        inet_pton(AF_INET, config->remote_ip, &tun->peer_addr.sin_addr);
        tun->peer_addr_valid = 1;
    } else {
        tun->peer_addr_valid = 0; /* Dynamic learning mode */
    }

    /* Initialize Poll Set */
    tun->poll_set = bitun_osal_poll_create();
    if (!tun->poll_set) {
        bitun_osal_socket_close(tun->udp_fd);
        return -1;
    }

    bitun_osal_poll_add(tun->poll_set, tun->udp_fd, BITUN_POLL_IN);

    /* Create DNS Queue */
    tun->dns_queue = bitun_osal_queue_create(sizeof(bitun_osal_dns_result_t), 16);
    if (!tun->dns_queue) {
        bitun_osal_poll_destroy(tun->poll_set);
        bitun_osal_socket_close(tun->udp_fd);
        return -1;
    }
    bitun_socket_t queue_fd = bitun_osal_queue_get_read_fd(tun->dns_queue);
    bitun_osal_poll_add(tun->poll_set, queue_fd, BITUN_POLL_IN);

    /* Open local TCP listener unconditionally if config->local_port > 0 */
    if (config->local_port > 0) {
        tun->tcp_listen_fd = bitun_osal_socket_create(AF_INET, SOCK_STREAM, 0);
        if (tun->tcp_listen_fd >= 0) {
            bitun_osal_socket_set_reuseaddr(tun->tcp_listen_fd);
            bitun_osal_socket_set_nonblocking(tun->tcp_listen_fd);

            struct sockaddr_in tcp_addr;
            memset(&tcp_addr, 0, sizeof(tcp_addr));
            tcp_addr.sin_family = AF_INET;
            tcp_addr.sin_port = htons(config->local_port);
            tcp_addr.sin_addr.s_addr = INADDR_ANY;

            if (bitun_osal_socket_bind(tun->tcp_listen_fd, (struct sockaddr *)&tcp_addr, sizeof(tcp_addr)) < 0) {
                perror("TCP listen bind failed");
                bitun_osal_socket_close(tun->tcp_listen_fd);
                bitun_osal_queue_destroy(tun->dns_queue);
                bitun_osal_poll_destroy(tun->poll_set);
                bitun_osal_socket_close(tun->udp_fd);
                return -1;
            }
            if (bitun_osal_socket_listen(tun->tcp_listen_fd, 10) < 0) {
                bitun_osal_socket_close(tun->tcp_listen_fd);
                bitun_osal_queue_destroy(tun->dns_queue);
                bitun_osal_poll_destroy(tun->poll_set);
                bitun_osal_socket_close(tun->udp_fd);
                return -1;
            }

            bitun_osal_poll_add(tun->poll_set, tun->tcp_listen_fd, BITUN_POLL_IN);
        }
    } else {
        tun->tcp_listen_fd = -1;
    }

    return 0;
}

/* Close channel and release FD */
static void close_channel(tunnel_t *tun, uint32_t channel_id) {
    int idx = -1;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (tun->channels[i].is_active && tun->channels[i].channel_id == channel_id) {
            idx = i;
            break;
        }
    }
    if (idx == -1) return;

    channel_t *ch = &tun->channels[idx];
    if (ch->tcp_fd >= 0) {
        bitun_osal_poll_del(tun->poll_set, ch->tcp_fd);
        bitun_osal_socket_close(ch->tcp_fd);
        ch->tcp_fd = -1;
    }
    socks5_free(&ch->socks5_ctx);
    ch->is_active = 0;
    printf("[Tunnel] Channel %u closed and cleaned up.\n", channel_id);
}

/* Send Control Frame over KCP */
static void send_control_frame(tunnel_t *tun, uint32_t channel_id, uint8_t cmd,
                               const uint8_t *payload, uint16_t payload_len) {
    if (!tun->kcp) return;
    
    uint16_t header_len = 8;
    uint8_t *frame = malloc(header_len + payload_len);
    if (!frame) return;

    uint32_t id_be = bitun_htobe32(channel_id);
    uint16_t len_be = bitun_htobe16(payload_len);

    memcpy(frame, &id_be, 4);
    frame[4] = cmd;
    frame[5] = 0x00; /* Reserved */
    memcpy(frame + 6, &len_be, 2);

    if (payload_len > 0 && payload) {
        memcpy(frame + 8, payload, payload_len);
    }

    ikcp_send(tun->kcp, (const char *)frame, header_len + payload_len);
    free(frame);
}

/* Send AUTH_RESET Frame */
static void send_auth_reset(tunnel_t *tun) {
    if (!tun->peer_addr_valid) return;
    handshake_reset_t rst;
    memcpy(rst.magic, MSG_RESET, 4);
    rst.timestamp = bitun_htobe64(bitun_osal_time_get_real_ms());
    rst.random_salt = bitun_htobe64(bitun_osal_random_u32());

    /* Signature target: magic + timestamp + salt + "BiTun Reset Request" */
    uint8_t data_to_sign[39];
    memcpy(data_to_sign, rst.magic, 4);
    memcpy(data_to_sign + 4, &rst.timestamp, 8);
    memcpy(data_to_sign + 12, &rst.random_salt, 8);
    memcpy(data_to_sign + 20, "BiTun Reset Request", 19);

    calculate_hmac(tun->config.psk, PSK_LEN, data_to_sign, sizeof(data_to_sign), rst.signature);

    bitun_osal_socket_sendto(tun->udp_fd, &rst, sizeof(rst), 0,
                             (struct sockaddr *)&tun->peer_addr, sizeof(tun->peer_addr));
    printf("[Handshake] Sent AUTH_RESET frame to Peer.\n");
}

/* Reset Tunnel State back to Disconnected */
static void reset_tunnel(tunnel_t *tun) {
    printf("[Tunnel] Resetting tunnel connection state...\n");
    tun->state = STATE_DISCONNECTED;
    if (tun->kcp) {
        ikcp_release(tun->kcp);
        tun->kcp = NULL;
    }
    memset(tun->session_key, 0, SESSION_KEY_LEN);
    anti_replay_init(&tun->anti_replay_win);
    
    tun->last_reset_timestamp = 0;
    tun->last_reset_salt = 0;

    /* Close all active channels */
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (tun->channels[i].is_active) {
            close_channel(tun, tun->channels[i].channel_id);
        }
    }

    if (tun->config.remote_ip && strcmp(tun->config.remote_ip, "0.0.0.0") != 0 && tun->config.remote_port > 0) {
        tun->peer_addr_valid = 1;
    } else {
        tun->peer_addr_valid = 0; /* Dynamic learning mode clears address on reset */
        memset(&tun->peer_addr, 0, sizeof(tun->peer_addr));
    }
}

/* Main Poll and Tick Loop */
void tunnel_run(tunnel_t *tun) {
    tun->running = 1;
    bitun_osal_event_t events[MAX_EVENTS];
    uint8_t read_buf[BUFFER_SIZE];

    printf("[Tunnel] Tunnel event loop started...\n");
    reset_tunnel(tun);

    while (tun->running) {
        if (g_should_exit) {
            tun->running = 0;
            break;
        }
        uint64_t now = bitun_osal_time_get_ms();
        uint32_t kcp_timeout = 20;

        if (tun->kcp) {
            ikcp_update(tun->kcp, (IUINT32)now);
            kcp_timeout = ikcp_check(tun->kcp, (IUINT32)now) - now;
            if (kcp_timeout > 50) kcp_timeout = 20;
            if (kcp_timeout <= 0) kcp_timeout = 5;
        }

        /* 1. State Machine Keepalives & Retransmissions */
        if (tun->state == STATE_CONNECTED) {
            if (now - tun->last_recv_time > 30000) {
                printf("[Tunnel] Peer timeout. Disconnecting.\n");
                reset_tunnel(tun);
            } else if (now - tun->last_keepalive_time > 10000) {
                send_control_frame(tun, 0, CMD_KEEPALIVE, NULL, 0);
                tun->last_keepalive_time = now;
            }
        } else if (tun->state == STATE_DISCONNECTED) {
            if (tun->peer_addr_valid) {
                tun->state = STATE_PUNCHING;
                tun->last_ping_time = 0;
            }
        } else if (tun->state == STATE_PUNCHING) {
            if (now - tun->last_ping_time > 1000) {
                /* Send signed PING */
                handshake_ping_pong_t ping;
                memcpy(ping.magic, MSG_PING, 4);
                ping.timestamp = bitun_htobe64(bitun_osal_time_get_real_ms());
                
                uint8_t data[12];
                memcpy(data, ping.magic, 4);
                memcpy(data + 4, &ping.timestamp, 8);
                calculate_hmac(tun->config.psk, PSK_LEN, data, 12, ping.signature);

                bitun_osal_socket_sendto(tun->udp_fd, &ping, sizeof(ping), 0,
                                         (struct sockaddr *)&tun->peer_addr, sizeof(tun->peer_addr));
                
                /* Also send AUTH_RESET frame for fast reconnection support */
                send_auth_reset(tun);

                tun->last_ping_time = now;
            }
        } else if (tun->state == STATE_AUTH) {
            if (now - tun->auth_start_time > 5000) {
                printf("[Handshake] Authentication handshake timeout.\n");
                reset_tunnel(tun);
            } else if (now - tun->last_ping_time > 500) {
                /* Retransmit AUTH_CHALLENGE */
                handshake_challenge_t chal;
                memcpy(chal.magic, MSG_CHAL, 4);
                memcpy(chal.random_salt, tun->r_local, 32);
                
                uint8_t data[36];
                memcpy(data, chal.magic, 4);
                memcpy(data + 4, chal.random_salt, 32);
                calculate_hmac(tun->config.psk, PSK_LEN, data, 36, chal.signature);

                bitun_osal_socket_sendto(tun->udp_fd, &chal, sizeof(chal), 0,
                                         (struct sockaddr *)&tun->peer_addr, sizeof(tun->peer_addr));
                tun->last_ping_time = now;
            }
        }

        /* 2. Process Backpressure Recovery & Activation */
        if (tun->kcp && ikcp_waitsnd(tun->kcp) < BACKPRESSURE_THRES / 2) {
            for (int idx = 0; idx < MAX_CHANNELS; idx++) {
                channel_t *ch = &tun->channels[idx];
                if (ch->is_active && ch->read_suspended && ch->tcp_fd >= 0) {
                    ch->read_suspended = 0;
                    bitun_osal_poll_mod(tun->poll_set, ch->tcp_fd, BITUN_POLL_IN);
                    printf("[Backpressure] Resumed and reactivated Poll read for channel %u.\n", ch->channel_id);
                }
            }
        }

        /* 3. OSAL Poll Multiplexing */
        int nfds = bitun_osal_poll_wait(tun->poll_set, kcp_timeout, events, MAX_EVENTS);
        for (int i = 0; i < nfds; i++) {
            bitun_socket_t fd = events[i].fd;

            if (fd == bitun_osal_queue_get_read_fd(tun->dns_queue)) {
                bitun_osal_queue_clear_wakeup(tun->dns_queue);
                bitun_osal_dns_result_t res;
                while (bitun_osal_queue_pop(tun->dns_queue, &res) == 0) {
                    int ch_idx = -1;
                    for (int c_idx = 0; c_idx < MAX_CHANNELS; c_idx++) {
                        if (tun->channels[c_idx].is_active && tun->channels[c_idx].channel_id == res.channel_id) {
                            ch_idx = c_idx;
                            break;
                        }
                    }
                    if (ch_idx != -1) {
                        channel_t *ch = &tun->channels[ch_idx];
                        if (res.success) {
                            int target_fd = bitun_osal_socket_create(AF_INET, SOCK_STREAM, 0);
                            if (target_fd >= 0) {
                                bitun_osal_socket_set_nonblocking(target_fd);
                                if (res.resolved_addr) {
                                    if (res.resolved_addr->sa_family == AF_INET) {
                                        ((struct sockaddr_in *)res.resolved_addr)->sin_port = htons(ch->port);
                                    } else if (res.resolved_addr->sa_family == AF_INET6) {
                                        ((struct sockaddr_in6 *)res.resolved_addr)->sin6_port = htons(ch->port);
                                    }
                                    printf("[DNS] Queue handler: Connecting to target for channel %u...\n", ch->channel_id);
                                    socklen_t addr_len = (res.resolved_addr->sa_family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
                                    int c_ret = bitun_osal_socket_connect(target_fd, res.resolved_addr, addr_len);
                                    if (c_ret == 0 || errno == EINPROGRESS) {
                                        ch->tcp_fd = target_fd;
                                        bitun_osal_poll_add(tun->poll_set, target_fd, BITUN_POLL_IN | BITUN_POLL_OUT);
                                    } else {
                                        bitun_osal_socket_close(target_fd);
                                        send_control_frame(tun, ch->channel_id, CMD_CONNECT_ACK, (uint8_t *)"\x01", 1);
                                        close_channel(tun, ch->channel_id);
                                    }
                                } else {
                                    struct sockaddr_in addr;
                                    memset(&addr, 0, sizeof(addr));
                                    addr.sin_family = AF_INET;
                                    memcpy(&addr.sin_addr.s_addr, res.resolved_ipv4, 4);
                                    addr.sin_port = htons(ch->port);
                                    printf("[DNS] Queue handler (IPv4): Connecting to target for channel %u...\n", ch->channel_id);
                                    int c_ret = bitun_osal_socket_connect(target_fd, (struct sockaddr *)&addr, sizeof(addr));
                                    if (c_ret == 0 || errno == EINPROGRESS) {
                                        ch->tcp_fd = target_fd;
                                        bitun_osal_poll_add(tun->poll_set, target_fd, BITUN_POLL_IN | BITUN_POLL_OUT);
                                    } else {
                                        bitun_osal_socket_close(target_fd);
                                        send_control_frame(tun, ch->channel_id, CMD_CONNECT_ACK, (uint8_t *)"\x01", 1);
                                        close_channel(tun, ch->channel_id);
                                    }
                                }
                            } else {
                                send_control_frame(tun, ch->channel_id, CMD_CONNECT_ACK, (uint8_t *)"\x01", 1);
                                close_channel(tun, ch->channel_id);
                            }
                        } else {
                            send_control_frame(tun, ch->channel_id, CMD_CONNECT_ACK, (uint8_t *)"\x01", 1);
                            close_channel(tun, ch->channel_id);
                        }
                    }
                    if (res.resolved_addr) {
                        free(res.resolved_addr);
                    }
                }
            } else if (fd == tun->udp_fd) {
                /* Receive raw UDP packets */
                struct sockaddr_in from;
                bitun_socklen_t from_len = sizeof(from);
                int n = bitun_osal_socket_recvfrom(tun->udp_fd, read_buf, BUFFER_SIZE, 0,
                                                  (struct sockaddr *)&from, &from_len);
                if (n <= 0) continue;

                /* Handle Handshake Magics (Symmetric Packet Multiplexing) */
                if (n >= 4) {
                    char magic[4];
                    memcpy(magic, read_buf, 4);

                    if (memcmp(magic, MSG_PING, 4) == 0) {
                        handshake_ping_pong_t *ping = (handshake_ping_pong_t *)read_buf;
                        uint8_t sig[32];
                        uint8_t data[12];
                        memcpy(data, ping->magic, 4);
                        memcpy(data + 4, &ping->timestamp, 8);
                        calculate_hmac(tun->config.psk, PSK_LEN, data, 12, sig);

                        if (const_memcmp(sig, ping->signature, 32) == 0) {
                            /* Valid PING */
                            if (!tun->peer_addr_valid) {
                                tun->peer_addr = from;
                                tun->peer_addr_valid = 1;
                                printf("[Handshake] Learned peer address from PING: %s:%d\n",
                                       inet_ntoa(from.sin_addr), ntohs(from.sin_port));
                            }
                            
                            /* Reply signed PONG */
                            handshake_ping_pong_t pong;
                            memcpy(pong.magic, MSG_PONG, 4);
                            pong.timestamp = ping->timestamp;
                            memcpy(data, pong.magic, 4);
                            memcpy(data + 4, &pong.timestamp, 8);
                            calculate_hmac(tun->config.psk, PSK_LEN, data, 12, pong.signature);

                            bitun_osal_socket_sendto(tun->udp_fd, &pong, sizeof(pong), 0,
                                                     (struct sockaddr *)&tun->peer_addr, sizeof(tun->peer_addr));

                            if (tun->state == STATE_DISCONNECTED || tun->state == STATE_PUNCHING) {
                                tun->state = STATE_AUTH;
                                /* Generate local challenge salt */
                                bitun_osal_random_bytes(tun->r_local, 32);
                                tun->auth_start_time = now;
                                tun->last_ping_time = 0; /* Trigger immediate AUTH_CHALLENGE */
                                printf("[Handshake] Transitioned to STATE_AUTH.\n");
                            }
                        }
                        continue;
                    } else if (memcmp(magic, MSG_PONG, 4) == 0) {
                        handshake_ping_pong_t *pong = (handshake_ping_pong_t *)read_buf;
                        uint8_t sig[32];
                        uint8_t data[12];
                        memcpy(data, pong->magic, 4);
                        memcpy(data + 4, &pong->timestamp, 8);
                        calculate_hmac(tun->config.psk, PSK_LEN, data, 12, sig);

                        if (const_memcmp(sig, pong->signature, 32) == 0) {
                            if (tun->state == STATE_PUNCHING) {
                                tun->state = STATE_AUTH;
                                bitun_osal_random_bytes(tun->r_local, 32);
                                tun->auth_start_time = now;
                                tun->last_ping_time = 0;
                                printf("[Handshake] Received valid PONG. Transitioned to STATE_AUTH.\n");
                            }
                        }
                        continue;
                    } else if (memcmp(magic, MSG_CHAL, 4) == 0) {
                        handshake_challenge_t *chal = (handshake_challenge_t *)read_buf;
                        uint8_t sig[32];
                        uint8_t data[36];
                        memcpy(data, chal->magic, 4);
                        memcpy(data + 4, chal->random_salt, 32);
                        calculate_hmac(tun->config.psk, PSK_LEN, data, 36, sig);

                        if (const_memcmp(sig, chal->signature, 32) == 0) {
                            memcpy(tun->r_remote, chal->random_salt, 32);
                            
                            /* Respond Challenge HMAC Response */
                            handshake_response_t resp;
                            memcpy(resp.magic, MSG_RESP, 4);
                            
                            /* Response = HMAC_PSK(R_local || R_remote || "BiTun Handshake Challenge") */
                            uint8_t resp_data[89];
                            memcpy(resp_data, tun->r_local, 32);
                            memcpy(resp_data + 32, tun->r_remote, 32);
                            memcpy(resp_data + 64, "BiTun Handshake Challenge", 25);
                            calculate_hmac(tun->config.psk, PSK_LEN, resp_data, sizeof(resp_data), resp.response);

                            bitun_osal_socket_sendto(tun->udp_fd, &resp, sizeof(resp), 0,
                                                     (struct sockaddr *)&tun->peer_addr, sizeof(tun->peer_addr));
                        }
                        continue;
                    } else if (memcmp(magic, MSG_RESP, 4) == 0) {
                        handshake_response_t *resp = (handshake_response_t *)read_buf;
                        
                        /* Expected Response = HMAC_PSK(R_remote || R_local || "BiTun Handshake Challenge") */
                        uint8_t expected_data[89];
                        memcpy(expected_data, tun->r_remote, 32);
                        memcpy(expected_data + 32, tun->r_local, 32);
                        memcpy(expected_data + 64, "BiTun Handshake Challenge", 25);
                        
                        uint8_t expected[32];
                        calculate_hmac(tun->config.psk, PSK_LEN, expected_data, sizeof(expected_data), expected);

                        if (const_memcmp(expected, resp->response, 32) == 0) {
                            /* Challenge successfully authenticated! */
                            if (tun->state == STATE_AUTH) {
                                printf("[Handshake] Authenticated Peer successfully!\n");
                                derive_session_key(tun->config.psk, PSK_LEN, tun->r_local, 32, tun->r_remote, 32, tun->session_key);
                                
                                /* Initialize KCP */
                                tun->kcp = ikcp_create(0x11223344, tun);
                                ikcp_setoutput(tun->kcp, kcp_output_cb);
                                ikcp_wndsize(tun->kcp, 32, 32);
                                ikcp_setmtu(tun->kcp, KCP_MTU);
                                ikcp_nodelay(tun->kcp, 1, 20, 2, 1);
                                
                                tun->state = STATE_CONNECTED;
                                tun->last_recv_time = now;
                                tun->last_keepalive_time = now;
                                printf("[Tunnel] Symmetric handshake complete. STATE_CONNECTED established.\n");
                            }
                        }
                        continue;
                    } else if (memcmp(magic, MSG_RESET, 4) == 0) {
                        /* Handle Authenticated Reset */
                        handshake_reset_t *rst = (handshake_reset_t *)read_buf;
                        uint64_t rst_time = bitun_be64toh(rst->timestamp);
                        uint64_t local_time = bitun_osal_time_get_real_ms();
                        
                        /* Timestamp window validation (±5 seconds) */
                        uint64_t diff = (local_time > rst_time) ? (local_time - rst_time) : (rst_time - local_time);
                        if (diff > 5000) {
                            printf("[Security] Blocked reset request: Timestamp skew too high (%lu ms)\n", diff);
                            continue;
                        }

                        /* HMAC signature validation */
                        uint8_t data_to_sign[39];
                        memcpy(data_to_sign, rst->magic, 4);
                        memcpy(data_to_sign + 4, &rst->timestamp, 8);
                        memcpy(data_to_sign + 12, &rst->random_salt, 8);
                        memcpy(data_to_sign + 20, "BiTun Reset Request", 19);

                        uint8_t sig[32];
                        calculate_hmac(tun->config.psk, PSK_LEN, data_to_sign, sizeof(data_to_sign), sig);

                        if (const_memcmp(sig, rst->signature, 32) == 0) {
                            uint64_t rst_salt = bitun_be64toh(rst->random_salt);
                            if (rst_time < tun->last_reset_timestamp || 
                                (rst_time == tun->last_reset_timestamp && rst_salt == tun->last_reset_salt)) {
                                printf("[Security] Reset frame replayed! Discarded.\n");
                                continue;
                            }
                            
                            tun->last_reset_timestamp = rst_time;
                            tun->last_reset_salt = rst_salt;

                            printf("[Handshake] Received Authenticated Reset. Resetting tunnel.\n");
                            reset_tunnel(tun);
                            
                            /* Adapt learn state immediately */
                            tun->peer_addr = from;
                            tun->peer_addr_valid = 1;
                            tun->state = STATE_AUTH;
                            bitun_osal_random_bytes(tun->r_local, 32);
                            tun->auth_start_time = now;
                            tun->last_ping_time = 0;
                        } else {
                            printf("[Security] Reset request HMAC verification failed!\n");
                        }
                        continue;
                    }
                }

                /* If not handshake magic, handle as AEAD encrypted KCP packet */
                if (tun->state == STATE_CONNECTED) {
                    if (n < 8 + AEAD_NONCE_LEN + AEAD_TAG_LEN) continue;
                    
                    uint64_t seq_be;
                    memcpy(&seq_be, read_buf, 8);
                    uint64_t seq = bitun_be64toh(seq_be);
                    
                    if (!anti_replay_check(&tun->anti_replay_win, seq)) continue;
                    
                    uint8_t nonce[AEAD_NONCE_LEN];
                    memcpy(nonce, read_buf + 8, AEAD_NONCE_LEN);
                    
                    uint8_t tag[AEAD_TAG_LEN];
                    memcpy(tag, read_buf + 8 + AEAD_NONCE_LEN, AEAD_TAG_LEN);
                    
                    int payload_offset = 8 + AEAD_NONCE_LEN + AEAD_TAG_LEN; // 36 bytes
                    int decrypted_len = bitun_osal_crypto_chacha20_poly1305_decrypt(
                        tun->session_key, nonce, 
                        read_buf + payload_offset, n - payload_offset, 
                        tag, read_buf + payload_offset
                    );
                    
                    if (decrypted_len < 0) {
                        /* Decrypt tag validation failure. Discard silently */
                        continue;
                    }

                    // Shift the decrypted payload back to the start of read_buf
                    memmove(read_buf, read_buf + payload_offset, decrypted_len);

                    anti_replay_update(&tun->anti_replay_win, seq);

                    /* Connection migration update: if source IP/Port changed, hot-swap */
                    if (from.sin_addr.s_addr != tun->peer_addr.sin_addr.s_addr ||
                        from.sin_port != tun->peer_addr.sin_port) {
                        tun->peer_addr = from;
                        printf("[Migration] Decrypted AEAD successfully from new address. Updated target endpoint to %s:%d\n",
                               inet_ntoa(from.sin_addr), ntohs(from.sin_port));
                    }

                    ikcp_input(tun->kcp, (const char *)read_buf, decrypted_len);
                    tun->last_recv_time = now;
                }
            } else if (fd == tun->tcp_listen_fd) {
                /* Accept TCP connections on our listening ports */
                struct sockaddr_in client_addr;
                bitun_socklen_t client_len = sizeof(client_addr);
                int client_fd = bitun_osal_socket_accept(tun->tcp_listen_fd, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd >= 0) {
                    bitun_osal_socket_set_nonblocking(client_fd);
                    
                    /* Find free channel slot */
                    int free_idx = -1;
                    for (int ch_idx = 0; ch_idx < MAX_CHANNELS; ch_idx++) {
                        if (!tun->channels[ch_idx].is_active) {
                            free_idx = ch_idx;
                            break;
                        }
                    }

                    if (free_idx != -1) {
                        channel_t *ch = &tun->channels[free_idx];
                        ch->is_active = 1;
                        ch->tcp_fd = client_fd;
                        ch->send_wnd = DEFAULT_WND_SIZE;
                        ch->recv_wnd = DEFAULT_WND_SIZE;
                        ch->local_recv_accum = 0;
                        ch->socks5_handshake_done = 0;
                        ch->read_suspended = 0;
                        socks5_init(&ch->socks5_ctx);

                        /* Generate Channel ID */
                        extern int g_is_odd_id_generator;
                        static uint32_t next_odd_id = 1;
                        static uint32_t next_even_id = 2;
                        if (g_is_odd_id_generator) {
                            ch->channel_id = next_odd_id;
                            next_odd_id += 2;
                        } else {
                            ch->channel_id = next_even_id;
                            next_even_id += 2;
                        }

                        bitun_osal_poll_add(tun->poll_set, client_fd, BITUN_POLL_IN);

                        printf("[SOCKS5] Accepted local SOCKS5 client. Handshake started. Channel ID: %u\n", ch->channel_id);
                    } else {
                        bitun_osal_socket_close(client_fd);
                    }
                }
            } else {
                /* Handles TCP client socket events (Local read / write / disconnect) */
                int ch_idx = -1;
                for (int c_idx = 0; c_idx < MAX_CHANNELS; c_idx++) {
                    if (tun->channels[c_idx].is_active && tun->channels[c_idx].tcp_fd == fd) {
                        ch_idx = c_idx;
                        break;
                    }
                }

                if (ch_idx != -1) {
                    channel_t *ch = &tun->channels[ch_idx];
                    
                    if (events[i].events & BITUN_POLL_OUT) {
                        int err = 0;
                        bitun_socklen_t len = sizeof(err);
                        if (getsockopt(events[i].fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
                            // Connection failed
                            send_control_frame(tun, ch->channel_id, CMD_CONNECT_ACK, (uint8_t *)"\x01", 1);
                            close_channel(tun, ch->channel_id);
                        } else {
                            // Connection successful
                            send_control_frame(tun, ch->channel_id, CMD_CONNECT_ACK, (uint8_t *)"\x00", 1);
                            // Switch to only listen for read events, prevent write events storm
                            bitun_osal_poll_mod(tun->poll_set, events[i].fd, BITUN_POLL_IN);
                        }
                        continue;
                    }

                    if (events[i].events & BITUN_POLL_ERR) {
                        close_channel(tun, ch->channel_id);
                        send_control_frame(tun, ch->channel_id, CMD_CLOSE, (uint8_t *)"\x02", 1);
                        continue;
                    }

                    if (events[i].events & BITUN_POLL_IN) {
                        if (ch->read_suspended) {
                            continue;
                        }
                        if (tun->kcp && ikcp_waitsnd(tun->kcp) >= BACKPRESSURE_THRES) {
                            for (int idx = 0; idx < MAX_CHANNELS; idx++) {
                                if (tun->channels[idx].is_active) {
                                    tun->channels[idx].read_suspended = 1;
                                }
                            }
                            continue;
                        }

                        int read_len = bitun_osal_socket_recv(fd, read_buf, BUFFER_SIZE, 0);
                        if (read_len > 0) {
                            if (!ch->socks5_handshake_done) {
                                /* Parse SOCKS5 Handshake Data */
                                uint8_t resp[BUFFER_SIZE];
                                size_t resp_len = 0;
                                int res = socks5_handle_input(&ch->socks5_ctx, read_buf, read_len, resp, &resp_len);
                                if (resp_len > 0) {
                                    bitun_osal_socket_send(fd, resp, resp_len, 0);
                                }
                                if (res == 1) {
                                    /* SOCKS5 Target address parsed successfully! Initiate target connection */
                                    ch->socks5_handshake_done = 1;
                                    ch->port = ch->socks5_ctx.port;
                                    
                                    /* Build CONNECT Frame to send to remote VPS endpoint */
                                    uint8_t payload[512];
                                    uint16_t port_be = bitun_htobe16(ch->port);
                                    
                                    if (ch->socks5_ctx.addr_type == 0x01) { // IPv4
                                        payload[0] = ADDR_TYPE_IPV4;
                                        memcpy(payload + 1, &port_be, 2);
                                        memcpy(payload + 3, ch->socks5_ctx.ipv4, 4);
                                        send_control_frame(tun, ch->channel_id, CMD_CONNECT, payload, 7);
                                    } else if (ch->socks5_ctx.addr_type == 0x03) { // Domain
                                        payload[0] = ADDR_TYPE_DOMAIN;
                                        memcpy(payload + 1, &port_be, 2);
                                        size_t dom_len = strlen(ch->socks5_ctx.domain);
                                        payload[3] = (uint8_t)dom_len;
                                        memcpy(payload + 4, ch->socks5_ctx.domain, dom_len);
                                        send_control_frame(tun, ch->channel_id, CMD_CONNECT, payload, 4 + dom_len);
                                    } else if (ch->socks5_ctx.addr_type == 0x04) { // IPv6
                                        payload[0] = ADDR_TYPE_IPV6;
                                        memcpy(payload + 1, &port_be, 2);
                                        memcpy(payload + 3, ch->socks5_ctx.ipv6, 16);
                                        send_control_frame(tun, ch->channel_id, CMD_CONNECT, payload, 19);
                                    }
                                } else if (res < 0) {
                                    /* SOCKS5 Error, close channel */
                                    close_channel(tun, ch->channel_id);
                                    send_control_frame(tun, ch->channel_id, CMD_CLOSE, (uint8_t *)"\x01", 1);
                                }
                            } else {
                                /* Standard Data Forwarding over KCP */
                                int to_read = (read_len > (int)ch->send_wnd) ? (int)ch->send_wnd : read_len;
                                if (to_read > 2048) to_read = 2048;

                                if (to_read > 0) {
                                    send_control_frame(tun, ch->channel_id, CMD_DATA, read_buf, to_read);
                                    ch->send_wnd -= to_read;
                                }
                            }
                        } else if (read_len == 0) {
                            /* TCP Client disconnected */
                            close_channel(tun, ch->channel_id);
                            send_control_frame(tun, ch->channel_id, CMD_CLOSE, (uint8_t *)"\x01", 1);
                        }
                    }
                }
            }
        }

        /* 4. Process KCP Received bytes */
        if (tun->state == STATE_CONNECTED && tun->kcp) {
            char kcp_buf[BUFFER_SIZE];
            int recv_len;
            while ((recv_len = ikcp_recv(tun->kcp, kcp_buf, BUFFER_SIZE)) > 0) {
                if (recv_len < 8) continue;
                
                uint32_t channel_id_be;
                memcpy(&channel_id_be, kcp_buf, 4);
                uint32_t channel_id = bitun_be32toh(channel_id_be);
                uint8_t cmd = kcp_buf[4];
                uint16_t payload_len_be;
                memcpy(&payload_len_be, kcp_buf + 6, 2);
                uint16_t payload_len = bitun_be16toh(payload_len_be);
                uint8_t *payload = (uint8_t *)kcp_buf + 8;

                if (cmd == CMD_KEEPALIVE) {
                    tun->last_recv_time = now;
                    continue;
                }

                /* Find or allocate channel */
                int ch_idx = -1;
                for (int c_idx = 0; c_idx < MAX_CHANNELS; c_idx++) {
                    if (tun->channels[c_idx].is_active && tun->channels[c_idx].channel_id == channel_id) {
                        ch_idx = c_idx;
                        break;
                    }
                }

                if (cmd == CMD_CONNECT) {
                    if (ch_idx != -1) continue; /* Already exists */

                    /* Allocate channel for incoming peer connection */
                    int free_idx = -1;
                    for (int c_idx = 0; c_idx < MAX_CHANNELS; c_idx++) {
                        if (!tun->channels[c_idx].is_active) {
                            free_idx = c_idx;
                            break;
                        }
                    }

                    if (free_idx != -1) {
                        channel_t *ch = &tun->channels[free_idx];
                        ch->is_active = 1;
                        ch->channel_id = channel_id;
                        ch->tcp_fd = -1;
                        ch->send_wnd = DEFAULT_WND_SIZE;
                        ch->recv_wnd = DEFAULT_WND_SIZE;
                        ch->local_recv_accum = 0;
                        ch->read_suspended = 0;

                        uint8_t addr_type = payload[0];
                        uint16_t target_port_be;
                        memcpy(&target_port_be, payload + 1, 2);
                        uint16_t target_port = bitun_be16toh(target_port_be);
                        ch->port = target_port;

                        ch->socks5_handshake_done = 1;

                        /* Connect to target address (IPv4 or Domain) */
                        if (addr_type == ADDR_TYPE_IPV4) {
                            struct sockaddr_in target_addr;
                            memset(&target_addr, 0, sizeof(target_addr));
                            target_addr.sin_family = AF_INET;
                            target_addr.sin_port = htons(target_port);
                            memcpy(&target_addr.sin_addr.s_addr, payload + 3, 4);

                            int target_fd = bitun_osal_socket_create(AF_INET, SOCK_STREAM, 0);
                            if (target_fd >= 0) {
                                bitun_osal_socket_set_nonblocking(target_fd);
                                printf("[Forward-R] Connecting to target IPv4 %s:%d...\n",
                                       inet_ntoa(target_addr.sin_addr), target_port);
                                
                                int c_ret = bitun_osal_socket_connect(target_fd, (struct sockaddr *)&target_addr, sizeof(target_addr));
                                if (c_ret == 0 || errno == EINPROGRESS) {
                                    ch->tcp_fd = target_fd;
                                    bitun_osal_poll_add(tun->poll_set, target_fd, BITUN_POLL_IN | BITUN_POLL_OUT);
                                } else {
                                    bitun_osal_socket_close(target_fd);
                                    send_control_frame(tun, channel_id, CMD_CONNECT_ACK, (uint8_t *)"\x01", 1);
                                    close_channel(tun, channel_id);
                                }
                            } else {
                                send_control_frame(tun, channel_id, CMD_CONNECT_ACK, (uint8_t *)"\x01", 1);
                                close_channel(tun, channel_id);
                            }
                        } else if (addr_type == ADDR_TYPE_DOMAIN) {
                            uint8_t dom_len = payload[3];
                            char *domain = malloc(dom_len + 1);
                            if (domain) {
                                memcpy(domain, payload + 4, dom_len);
                                domain[dom_len] = '\0';
                                printf("[DNS] Dispatching async resolution for domain: %s\n", domain);
                                bitun_osal_dns_resolve_async(domain, channel_id, tun->dns_queue);
                                free(domain);
                            }
                        }
                    } else {
                        send_control_frame(tun, channel_id, CMD_CONNECT_ACK, (uint8_t *)"\x01", 1);
                    }
                } else if (ch_idx != -1) {
                    channel_t *ch = &tun->channels[ch_idx];

                    if (cmd == CMD_CONNECT_ACK) {
                        uint8_t status = payload[0];
                        if (status == 0x00) {
                            printf("[Channel] Channel %u Target connected successfully!\n", channel_id);
                            ch->socks5_handshake_done = 1;
                            uint8_t socks5_resp[10] = {0x05, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
                            bitun_osal_socket_send(ch->tcp_fd, socks5_resp, 10, 0);
                            bitun_osal_poll_add(tun->poll_set, ch->tcp_fd, BITUN_POLL_IN);
                        } else {
                            printf("[Channel] Channel %u target connection refused.\n", channel_id);
                            uint8_t socks5_resp[10] = {0x05, 0x05, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
                            bitun_osal_socket_send(ch->tcp_fd, socks5_resp, 10, 0);
                            close_channel(tun, channel_id);
                        }
                    } else if (cmd == CMD_DATA) {
                        if (ch->tcp_fd >= 0) {
                            int sent = bitun_osal_socket_send(ch->tcp_fd, payload, payload_len, 0);
                            if (sent > 0) {
                                ch->local_recv_accum += sent;
                                /* Window update trigger: if consumed 2KB, report update */
                                if (ch->local_recv_accum >= 2048) {
                                    uint32_t accum_be = bitun_htobe32(ch->local_recv_accum);
                                    send_control_frame(tun, channel_id, CMD_WINDOW_UPDATE, (uint8_t *)&accum_be, 4);
                                    ch->local_recv_accum = 0;
                                }
                            }
                        }
                    } else if (cmd == CMD_CLOSE) {
                        close_channel(tun, channel_id);
                    } else if (cmd == CMD_WINDOW_UPDATE) {
                        uint32_t window_inc;
                        memcpy(&window_inc, payload, 4);
                        ch->send_wnd += bitun_be32toh(window_inc);
                        
                        /* Resume reading if it was suspended */
                        if (ch->tcp_fd >= 0) {
                            bitun_osal_poll_mod(tun->poll_set, ch->tcp_fd, BITUN_POLL_IN);
                        }
                    }
                }
            }
        }
    }
}

/* Destroy Tunnel */
void tunnel_destroy(tunnel_t *tun) {
    tun->running = 0;
    bitun_osal_mutex_lock(tun->mutex);
    reset_tunnel(tun);
    if (tun->udp_fd >= 0) bitun_osal_socket_close(tun->udp_fd);
    if (tun->tcp_listen_fd >= 0) bitun_osal_socket_close(tun->tcp_listen_fd);
    if (tun->poll_set) {
        bitun_osal_poll_destroy(tun->poll_set);
        tun->poll_set = NULL;
    }
    if (tun->dns_queue) {
        bitun_osal_dns_result_t res;
        while (bitun_osal_queue_pop(tun->dns_queue, &res) == 0) {
            if (res.resolved_addr) {
                free(res.resolved_addr);
            }
        }
        bitun_osal_queue_destroy(tun->dns_queue);
        tun->dns_queue = NULL;
    }
    bitun_osal_mutex_unlock(tun->mutex);
    bitun_osal_mutex_destroy(tun->mutex);
    printf("[Tunnel] Destroyed.\n");
}
