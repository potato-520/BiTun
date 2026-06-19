#ifndef BITUN_TUNNEL_H
#define BITUN_TUNNEL_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#ifdef __linux__
#include <netinet/in.h>
#else
#include <sys/socket.h>
#endif
#include "bitun_osal.h"
#include "encrypt.h"
#include "socks5.h"
#include "ikcp.h"

#define MAX_CHANNELS 256
#define BUFFER_SIZE 65536
#define KCP_MTU 1400

/* Tunnel Handshake States */
typedef enum {
    STATE_DISCONNECTED = 0,
    STATE_PUNCHING,
    STATE_AUTH,
    STATE_CONNECTED
} tunnel_state_t;

/* Channel Mapping Modes */
typedef enum {
    MODE_SOCKS5 = 0,      /* Dynamic remote forwarding */
    MODE_FORWARD_L,       /* Local static port forwarding (-L) */
    MODE_FORWARD_R        /* Remote static port forwarding (-R) */
} mapping_mode_t;

/* Channel Structure */
typedef struct {
    uint32_t channel_id;
    int tcp_fd;           /* Local TCP connection FD */
    int is_active;        /* Is this channel slot in use? */
    socks5_context_t socks5_ctx;
    uint32_t send_wnd;    /* Available sending window in bytes */
    uint32_t recv_wnd;    /* Available receiving window in bytes */
    uint32_t local_recv_accum; /* Bytes read from TCP and written to KCP since last window update */
    int socks5_handshake_done;
    uint16_t port;         /* Target port */
    int read_suspended;
} channel_t;

/* Configuration parameters */
typedef struct {
    char *local_ip;
    int local_port;
    char *remote_ip;
    int remote_port;
    char *target_ip;
    int target_port;
    mapping_mode_t mode;
    uint8_t psk[PSK_LEN];
} tunnel_config_t;

// dns_result_t is replaced by bitun_osal_dns_result_t from bitun_osal.h

/* Global Tunnel Context */
typedef struct {
    tunnel_config_t config;
    tunnel_state_t state;
    
    int udp_fd;
    struct sockaddr_in local_addr;
    struct sockaddr_in peer_addr;
    int peer_addr_valid;
    
    uint8_t r_local[32];
    uint8_t r_remote[32];
    uint8_t session_key[SESSION_KEY_LEN];
    
    uint64_t seq_send;
    anti_replay_window_t anti_replay_win;
    
    ikcpcb *kcp;
    
    channel_t channels[MAX_CHANNELS];
    bitun_osal_poll_set_t *poll_set;
    int tcp_listen_fd;
    
    uint64_t last_recv_time;
    uint64_t last_keepalive_time;
    uint64_t last_ping_time;
    uint64_t auth_start_time;
    
    bitun_osal_mutex_t *mutex;
    pthread_t thread_id;
    int running;

    bitun_osal_queue_t *dns_queue;
    uint64_t last_reset_timestamp;
    uint64_t last_reset_salt;
} tunnel_t;

/* Protocol Commands (Cmd Type) */
#define CMD_CONNECT        0x01
#define CMD_CONNECT_ACK    0x02
#define CMD_DATA           0x03
#define CMD_CLOSE          0x04
#define CMD_KEEPALIVE      0x05
#define CMD_WINDOW_UPDATE  0x06

/* Address Type in CMD_CONNECT */
#define ADDR_TYPE_IPV4    0x01
#define ADDR_TYPE_DOMAIN  0x02
#define ADDR_TYPE_IPV6    0x03
#define ADDR_TYPE_SOCKS5  0x04

/* Handshake Messages Magic */
#define MSG_PING "PING"
#define MSG_PONG "PONG"
#define MSG_CHAL "CHAL"
#define MSG_RESP "RESP"
#define MSG_RESET "RESET"

/* Handshake packets struct (Sent as raw UDP) */
typedef struct {
    char magic[4];
    uint64_t timestamp;
    uint8_t signature[32];
} handshake_ping_pong_t;

typedef struct {
    char magic[4];
    uint8_t random_salt[32];
    uint8_t signature[32];
} handshake_challenge_t;

typedef struct {
    char magic[4];
    uint8_t response[32];
} handshake_response_t;

typedef struct {
    char magic[4];
    uint64_t timestamp;
    uint64_t random_salt;
    uint8_t signature[32];
} handshake_reset_t;

/* Public API */
int tunnel_init(tunnel_t *tun, const tunnel_config_t *config);
void tunnel_run(tunnel_t *tun);
void tunnel_destroy(tunnel_t *tun);

#endif /* BITUN_TUNNEL_H */
