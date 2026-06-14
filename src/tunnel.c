#define _GNU_SOURCE
#include "tunnel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <pthread.h>

#define MAX_EVENTS 1024
#define DEFAULT_WND_SIZE 4096  /* 4KB Channel Window */
#define BACKPRESSURE_THRES 32  /* 32 KCP Packets */

/* DNS Request Context to prevent UAF during async resolution */
typedef struct {
    char *domain;
    uint32_t channel_id;
    tunnel_t *tun;
    pthread_t thread;
    struct sockaddr_in resolved_addr;
    int success;
    int done;
} DNS_RequestContext;

static int const_memcmp(const void *a, const void *b, size_t len) {
    const unsigned char *ua = a;
    const unsigned char *ub = b;
    unsigned char diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (ua[i] ^ ub[i]);
    }
    return diff != 0;
}

/* Helper to set non-blocking on a socket */
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Helper to get monotonic time in milliseconds */
static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Helper to get wall-clock time in milliseconds */
static uint64_t get_real_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Global array to track active DNS requests for teardown */
static DNS_RequestContext *active_dns_reqs[MAX_CHANNELS] = {NULL};
static pthread_mutex_t dns_mutex = PTHREAD_MUTEX_INITIALIZER;

/* KCP Output Callback - Encrypts and sends over UDP */
static int kcp_output_cb(const char *buf, int len, ikcpcb *kcp, void *user) {
    (void)kcp;
    tunnel_t *tun = (tunnel_t *)user;
    if (!tun->peer_addr_valid) return 0;

    uint64_t seq = ++tun->seq_send;
    uint8_t nonce[AEAD_NONCE_LEN];
    uint64_t seq_be = htobe64(seq);
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

    sendto(tun->udp_fd, packet, out_len, 0,
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
    pthread_mutex_init(&tun->mutex, NULL);
    
    /* Open UDP socket */
    tun->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (tun->udp_fd < 0) return -1;
    set_nonblocking(tun->udp_fd);

    memset(&tun->local_addr, 0, sizeof(tun->local_addr));
    tun->local_addr.sin_family = AF_INET;
    tun->local_addr.sin_port = htons(config->local_port);
    if (config->local_ip) {
        inet_pton(AF_INET, config->local_ip, &tun->local_addr.sin_addr);
    } else {
        tun->local_addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(tun->udp_fd, (struct sockaddr *)&tun->local_addr, sizeof(tun->local_addr)) < 0) {
        close(tun->udp_fd);
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

    /* Initialize Epoll */
    tun->epoll_fd = epoll_create1(0);
    if (tun->epoll_fd < 0) {
        close(tun->udp_fd);
        return -1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = tun->udp_fd;
    epoll_ctl(tun->epoll_fd, EPOLL_CTL_ADD, tun->udp_fd, &ev);

    /* Create DNS self-pipe */
    if (pipe(tun->dns_pipe_fd) < 0) {
        close(tun->epoll_fd);
        close(tun->udp_fd);
        return -1;
    }
    set_nonblocking(tun->dns_pipe_fd[0]);
    set_nonblocking(tun->dns_pipe_fd[1]);

    struct epoll_event dns_ev;
    dns_ev.events = EPOLLIN;
    dns_ev.data.fd = tun->dns_pipe_fd[0];
    epoll_ctl(tun->epoll_fd, EPOLL_CTL_ADD, tun->dns_pipe_fd[0], &dns_ev);

    /* Open local TCP listener if we are forwarding locally or SOCKS5 on this side */
    if (config->mode == MODE_SOCKS5 || config->mode == MODE_FORWARD_L) {
        tun->tcp_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (tun->tcp_listen_fd >= 0) {
            int opt = 1;
            setsockopt(tun->tcp_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            set_nonblocking(tun->tcp_listen_fd);

            struct sockaddr_in tcp_addr;
            memset(&tcp_addr, 0, sizeof(tcp_addr));
            tcp_addr.sin_family = AF_INET;
            tcp_addr.sin_port = htons(config->local_port);
            tcp_addr.sin_addr.s_addr = INADDR_ANY;

            if (bind(tun->tcp_listen_fd, (struct sockaddr *)&tcp_addr, sizeof(tcp_addr)) < 0) {
                perror("TCP listen bind failed");
                close(tun->tcp_listen_fd);
                close(tun->epoll_fd);
                close(tun->udp_fd);
                close(tun->dns_pipe_fd[0]);
                close(tun->dns_pipe_fd[1]);
                return -1;
            }
            if (listen(tun->tcp_listen_fd, 10) < 0) {
                close(tun->tcp_listen_fd);
                close(tun->epoll_fd);
                close(tun->udp_fd);
                close(tun->dns_pipe_fd[0]);
                close(tun->dns_pipe_fd[1]);
                return -1;
            }

            ev.events = EPOLLIN;
            ev.data.fd = tun->tcp_listen_fd;
            epoll_ctl(tun->epoll_fd, EPOLL_CTL_ADD, tun->tcp_listen_fd, &ev);
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
        epoll_ctl(tun->epoll_fd, EPOLL_CTL_DEL, ch->tcp_fd, NULL);
        close(ch->tcp_fd);
        ch->tcp_fd = -1;
    }
    socks5_free(&ch->socks5_ctx);
    ch->is_active = 0;
    printf("[Tunnel] Channel %u closed and cleaned up.\n", channel_id);

    /* Nullify any active DNS request pointers to prevent UAF */
    pthread_mutex_lock(&dns_mutex);
    if (active_dns_reqs[idx]) {
        active_dns_reqs[idx]->tun = NULL; /* Detach channel pointer */
    }
    pthread_mutex_unlock(&dns_mutex);
}

/* Send Control Frame over KCP */
static void send_control_frame(tunnel_t *tun, uint32_t channel_id, uint8_t cmd,
                               const uint8_t *payload, uint16_t payload_len) {
    if (!tun->kcp) return;
    
    uint16_t header_len = 8;
    uint8_t *frame = malloc(header_len + payload_len);
    if (!frame) return;

    uint32_t id_be = htobe32(channel_id);
    uint16_t len_be = htobe16(payload_len);

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

/* Safe Async DNS Resolver Thread Function */
static void *dns_resolve_thread(void *arg) {
    DNS_RequestContext *ctx = (DNS_RequestContext *)arg;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    printf("[DNS] Resolving %s asynchronously...\n", ctx->domain);
    int ret = getaddrinfo(ctx->domain, NULL, &hints, &res);
    
    pthread_mutex_lock(&dns_mutex);
    if (ctx->tun != NULL) {
        dns_result_t result;
        result.channel_id = ctx->channel_id;
        result.success = 0;
        memset(&result.addr, 0, sizeof(result.addr));
        
        if (ret == 0 && res != NULL) {
            result.addr = *(struct sockaddr_in *)res->ai_addr;
            result.success = 1;
        }
        if (write(ctx->tun->dns_pipe_fd[1], &result, sizeof(dns_result_t)) < 0) {
            perror("[DNS] write to dns_pipe_fd failed");
        }
    } else {
        printf("[DNS] Resolution finished but channel/tunnel is gone.\n");
    }
    pthread_mutex_unlock(&dns_mutex);

    if (res) freeaddrinfo(res);
    
    free(ctx->domain);
    free(ctx);
    return NULL;
}

/* Send AUTH_RESET Frame */
static void send_auth_reset(tunnel_t *tun) {
    if (!tun->peer_addr_valid) return;
    handshake_reset_t rst;
    memcpy(rst.magic, MSG_RESET, 4);
    rst.timestamp = htobe64(get_real_time_ms());
    rst.random_salt = htobe64(random());

    /* Signature target: magic + timestamp + salt + "BiTun Reset Request" */
    uint8_t data_to_sign[39];
    memcpy(data_to_sign, rst.magic, 4);
    memcpy(data_to_sign + 4, &rst.timestamp, 8);
    memcpy(data_to_sign + 12, &rst.random_salt, 8);
    memcpy(data_to_sign + 20, "BiTun Reset Request", 19);

    calculate_hmac(tun->config.psk, PSK_LEN, data_to_sign, sizeof(data_to_sign), rst.signature);

    sendto(tun->udp_fd, &rst, sizeof(rst), 0,
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

    /* Clean up active DNS requests to prevent UAF or leaking */
    pthread_mutex_lock(&dns_mutex);
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (active_dns_reqs[i] != NULL) {
            active_dns_reqs[i]->tun = NULL;
            active_dns_reqs[i] = NULL;
        }
    }
    pthread_mutex_unlock(&dns_mutex);

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

/* Main Epoll and Tick Loop */
void tunnel_run(tunnel_t *tun) {
    tun->running = 1;
    struct epoll_event events[MAX_EVENTS];
    uint8_t read_buf[BUFFER_SIZE];

    printf("[Tunnel] Tunnel event loop started...\n");
    reset_tunnel(tun);

    while (tun->running) {
        uint64_t now = get_time_ms();
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
                ping.timestamp = htobe64(get_real_time_ms());
                
                uint8_t data[12];
                memcpy(data, ping.magic, 4);
                memcpy(data + 4, &ping.timestamp, 8);
                calculate_hmac(tun->config.psk, PSK_LEN, data, 12, ping.signature);

                sendto(tun->udp_fd, &ping, sizeof(ping), 0,
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

                sendto(tun->udp_fd, &chal, sizeof(chal), 0,
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
                    struct epoll_event ev;
                    ev.events = EPOLLIN | EPOLLET;
                    ev.data.fd = ch->tcp_fd;
                    epoll_ctl(tun->epoll_fd, EPOLL_CTL_MOD, ch->tcp_fd, &ev);
                    printf("[Backpressure] Resumed and reactivated Epoll read for channel %u.\n", ch->channel_id);
                }
            }
        }

        /* 3. Epoll I/O multiplexing */
        int nfds = epoll_wait(tun->epoll_fd, events, MAX_EVENTS, kcp_timeout);
        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == tun->dns_pipe_fd[0]) {
                dns_result_t res;
                while (read(tun->dns_pipe_fd[0], &res, sizeof(dns_result_t)) > 0) {
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
                            int target_fd = socket(AF_INET, SOCK_STREAM, 0);
                            if (target_fd >= 0) {
                                set_nonblocking(target_fd);
                                res.addr.sin_port = htons(ch->port);
                                printf("[DNS] Pipe handler: Connecting to target for channel %u...\n", ch->channel_id);
                                int c_ret = connect(target_fd, (struct sockaddr *)&res.addr, sizeof(res.addr));
                                if (c_ret == 0 || errno == EINPROGRESS) {
                                    ch->tcp_fd = target_fd;
                                    struct epoll_event ev;
                                    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
                                    ev.data.fd = target_fd;
                                    epoll_ctl(tun->epoll_fd, EPOLL_CTL_ADD, target_fd, &ev);
                                } else {
                                    close(target_fd);
                                    send_control_frame(tun, ch->channel_id, CMD_CONNECT_ACK, (uint8_t *)"\x01", 1);
                                    close_channel(tun, ch->channel_id);
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
                }
            } else if (fd == tun->udp_fd) {
                /* Receive raw UDP packets */
                struct sockaddr_in from;
                socklen_t from_len = sizeof(from);
                int n = recvfrom(tun->udp_fd, read_buf, BUFFER_SIZE, 0,
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

                            sendto(tun->udp_fd, &pong, sizeof(pong), 0,
                                   (struct sockaddr *)&tun->peer_addr, sizeof(tun->peer_addr));

                            if (tun->state == STATE_DISCONNECTED || tun->state == STATE_PUNCHING) {
                                tun->state = STATE_AUTH;
                                /* Generate local challenge salt */
                                for (int k = 0; k < 32; k++) tun->r_local[k] = random() & 0xFF;
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
                                for (int k = 0; k < 32; k++) tun->r_local[k] = random() & 0xFF;
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

                            sendto(tun->udp_fd, &resp, sizeof(resp), 0,
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
                        uint64_t rst_time = be64toh(rst->timestamp);
                        uint64_t local_time = get_real_time_ms();
                        
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
                            uint64_t rst_salt = be64toh(rst->random_salt);
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
                            for (int k = 0; k < 32; k++) tun->r_local[k] = random() & 0xFF;
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
                    uint64_t seq = be64toh(seq_be);
                    
                    if (!anti_replay_check(&tun->anti_replay_win, seq)) continue;
                    
                    uint8_t nonce[AEAD_NONCE_LEN];
                    memcpy(nonce, read_buf + 8, AEAD_NONCE_LEN);
                    
                    uint8_t tag[AEAD_TAG_LEN];
                    memcpy(tag, read_buf + 8 + AEAD_NONCE_LEN, AEAD_TAG_LEN);
                    
                    int decrypted_len = decrypt_chacha20_poly1305(tun->session_key, nonce,
                                                                  read_buf + 8 + AEAD_NONCE_LEN + AEAD_TAG_LEN,
                                                                  n - (8 + AEAD_NONCE_LEN + AEAD_TAG_LEN),
                                                                  tag, read_buf);
                    if (decrypted_len < 0) {
                        /* Decrypt tag validation failure. Discard silently (Defect 4) */
                        continue;
                    }

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
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(tun->tcp_listen_fd, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd >= 0) {
                    set_nonblocking(client_fd);
                    
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

                        /* Generate Channel ID. Even if client-side, Odd if server-side SOCKS5 */
                        /* We distinguish using the local_port of the config: if local_port == client, odd, etc. */
                        /* In PC simulator, we let the process command args define if we generate odd or even IDs */
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

                        struct epoll_event client_ev;
                        client_ev.events = EPOLLIN | EPOLLET;
                        client_ev.data.fd = client_fd;
                        epoll_ctl(tun->epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev);

                        if (tun->config.mode == MODE_SOCKS5) {
                            /* SOCKS5 proxy: wait for handshake before triggering remote connect */
                            printf("[SOCKS5] Accepted local SOCKS5 client. Handshake started. Channel ID: %u\n", ch->channel_id);
                        } else {
                            /* Local Static Port Forwarding L -> Connect to target */
                            printf("[Forward-L] Accepted client connection. Sending CMD_CONNECT for Channel ID: %u\n", ch->channel_id);
                            
                            /* Payload: IPV4 + Target Port + IP */
                            uint8_t payload[7];
                            payload[0] = ADDR_TYPE_IPV4;
                            uint16_t port_be = htobe16(tun->config.target_port);
                            memcpy(payload + 1, &port_be, 2);
                            struct in_addr ip_addr;
                            inet_pton(AF_INET, tun->config.target_ip, &ip_addr);
                            memcpy(payload + 3, &ip_addr.s_addr, 4);

                            send_control_frame(tun, ch->channel_id, CMD_CONNECT, payload, 7);
                        }
                    } else {
                        close(client_fd);
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
                    
                    if (events[i].events & EPOLLERR) {
                        close_channel(tun, ch->channel_id);
                        send_control_frame(tun, ch->channel_id, CMD_CLOSE, (uint8_t *)"\x02", 1);
                        continue;
                    }

                    if (events[i].events & EPOLLIN) {
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

                        int read_len = recv(fd, read_buf, BUFFER_SIZE, 0);
                        if (read_len > 0) {
                            if (tun->config.mode == MODE_SOCKS5 && !ch->socks5_handshake_done) {
                                /* Parse SOCKS5 Handshake Data */
                                uint8_t resp[BUFFER_SIZE];
                                size_t resp_len = 0;
                                int res = socks5_handle_input(&ch->socks5_ctx, read_buf, read_len, resp, &resp_len);
                                if (resp_len > 0) {
                                    send(fd, resp, resp_len, 0);
                                }
                                if (res == 1) {
                                    /* SOCKS5 Target address parsed successfully! Initiate target connection */
                                    ch->socks5_handshake_done = 1;
                                    ch->port = ch->socks5_ctx.port;
                                    
                                    /* Build CONNECT Frame to send to remote VPS endpoint */
                                    uint8_t payload[512];
                                    uint16_t port_be = htobe16(ch->port);
                                    
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
                                /* Flow Control (Defect 5): Check Channel Window Size */
                                int to_read = (read_len > (int)ch->send_wnd) ? (int)ch->send_wnd : read_len;
                                /* Quota check (Defect 2): Limit single read size to 2KB */
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
                uint32_t channel_id = be32toh(channel_id_be);
                uint8_t cmd = kcp_buf[4];
                uint16_t payload_len_be;
                memcpy(&payload_len_be, kcp_buf + 6, 2);
                uint16_t payload_len = be16toh(payload_len_be);
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
                        uint16_t target_port = be16toh(target_port_be);
                        ch->port = target_port;

                        if (addr_type == ADDR_TYPE_SOCKS5) {
                            /* remote wants SOCKS5. Initialize and ACK immediately */
                            ch->socks5_handshake_done = 0;
                            socks5_init(&ch->socks5_ctx);
                            send_control_frame(tun, channel_id, CMD_CONNECT_ACK, (uint8_t *)"\x00", 1);
                        } else {
                            /* Connect to target address (IPv4 or Domain) */
                            if (addr_type == ADDR_TYPE_IPV4) {
                                struct sockaddr_in target_addr;
                                memset(&target_addr, 0, sizeof(target_addr));
                                target_addr.sin_family = AF_INET;
                                target_addr.sin_port = htons(target_port);
                                memcpy(&target_addr.sin_addr.s_addr, payload + 3, 4);

                                int target_fd = socket(AF_INET, SOCK_STREAM, 0);
                                if (target_fd >= 0) {
                                    set_nonblocking(target_fd);
                                    printf("[Forward-R] Connecting to target IPv4 %s:%d...\n",
                                           inet_ntoa(target_addr.sin_addr), target_port);
                                    
                                    int c_ret = connect(target_fd, (struct sockaddr *)&target_addr, sizeof(target_addr));
                                    if (c_ret == 0 || errno == EINPROGRESS) {
                                        ch->tcp_fd = target_fd;
                                        struct epoll_event ev;
                                        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
                                        ev.data.fd = target_fd;
                                        epoll_ctl(tun->epoll_fd, EPOLL_CTL_ADD, target_fd, &ev);
                                    } else {
                                        close(target_fd);
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
                                memcpy(domain, payload + 4, dom_len);
                                domain[dom_len] = '\0';

                                /* Trigger Asynchronous Safe DNS Resolution to avoid blocking loop */
                                DNS_RequestContext *dns_ctx = malloc(sizeof(DNS_RequestContext));
                                dns_ctx->domain = domain;
                                dns_ctx->channel_id = channel_id;
                                dns_ctx->tun = tun;
                                dns_ctx->success = 0;
                                dns_ctx->done = 0;

                                pthread_mutex_lock(&dns_mutex);
                                active_dns_reqs[free_idx] = dns_ctx;
                                pthread_mutex_unlock(&dns_mutex);

                                pthread_create(&dns_ctx->thread, NULL, dns_resolve_thread, dns_ctx);
                                pthread_detach(dns_ctx->thread);
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
                            
                            /* Ensure epoll checks read events since connection is now active */
                            struct epoll_event ev;
                            ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
                            ev.data.fd = ch->tcp_fd;
                            epoll_ctl(tun->epoll_fd, EPOLL_CTL_MOD, ch->tcp_fd, &ev);
                        } else {
                            printf("[Channel] Channel %u target connection refused.\n", channel_id);
                            close_channel(tun, channel_id);
                        }
                    } else if (cmd == CMD_DATA) {
                        if (ch->tcp_fd >= 0) {
                            int sent = send(ch->tcp_fd, payload, payload_len, 0);
                            if (sent > 0) {
                                ch->local_recv_accum += sent;
                                /* Window update trigger: if consumed 2KB, report update */
                                if (ch->local_recv_accum >= 2048) {
                                    uint32_t accum_be = htobe32(ch->local_recv_accum);
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
                        ch->send_wnd += be32toh(window_inc);
                        
                        /* Resume reading if it was suspended */
                        struct epoll_event ev;
                        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
                        ev.data.fd = ch->tcp_fd;
                        epoll_ctl(tun->epoll_fd, EPOLL_CTL_MOD, ch->tcp_fd, &ev);
                    }
                }
            }
        }
    }
}

/* Destroy Tunnel */
void tunnel_destroy(tunnel_t *tun) {
    tun->running = 0;
    pthread_mutex_lock(&tun->mutex);
    reset_tunnel(tun);
    if (tun->udp_fd >= 0) close(tun->udp_fd);
    if (tun->tcp_listen_fd >= 0) close(tun->tcp_listen_fd);
    close(tun->epoll_fd);
    close(tun->dns_pipe_fd[0]);
    close(tun->dns_pipe_fd[1]);
    pthread_mutex_unlock(&tun->mutex);
    pthread_mutex_destroy(&tun->mutex);
    printf("[Tunnel] Destroyed.\n");
}
