#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "tunnel.h"
#include "bitun_osal.h"

/* Global odd/even ID generator flag */
int g_is_odd_id_generator = 0;

static tunnel_t g_tun;
volatile sig_atomic_t g_should_exit = 0;

static void handle_signal(int sig) {
    (void)sig;
    g_should_exit = 1;
}

static void print_usage(const char *prog) {
    printf("BiTun Symmetrical Tunnel Simulator - PC Version\n");
    printf("Usage:\n");
    printf("  %s -m <mode> -p <local_port> [-r <remote_ip:remote_port>] [-t <target_ip:target_port>] -k <psk> [--odd | --even]\n\n", prog);
    printf("Options:\n");
    printf("  -m, --mode      Mode of operation: socks5, forward_l (Local mapping), forward_r (Remote mapping)\n");
    printf("  -p, --port      Local port to bind to (e.g. 8080 for SOCKS5 or forwarding)\n");
    printf("  -r, --remote    Remote peer UDP endpoint in IP:Port format. Omit for dynamic passive learning mode.\n");
    printf("  -t, --target    Target connection endpoint in IP:Port format (required for forward_l and forward_r)\n");
    printf("  -k, --psk       Pre-shared key (exactly 32 characters, or padded/truncated to 32 bytes)\n");
    printf("  --odd           Configure this process to generate ODD channel IDs (default)\n");
    printf("  --even          Configure this process to generate EVEN channel IDs\n");
    printf("  -h, --help      Show this help information\n\n");
    printf("Examples:\n");
    printf("  # Start Peer A (SOCKS5 Proxy Listener, generating odd channel IDs, waiting for Peer B connection):\n");
    printf("  %s -m socks5 -p 1080 -k MySecretPSKKey123456789012345678 --odd\n\n", prog);
    printf("  # Start Peer B (Dynamic client executor, connecting to Peer A on local UDP port 9000):\n");
    printf("  %s -m forward_r -p 9000 -r 127.0.0.1:1080 -k MySecretPSKKey123456789012345678 --even\n", prog);
}

int main(int argc, char **argv) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    tunnel_config_t config;
    memset(&config, 0, sizeof(config));
    config.mode = MODE_SOCKS5;
    
    char *remote_str = NULL;
    char *target_str = NULL;
    char *psk_str = NULL;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--mode") == 0) {
            if (++i >= argc) { print_usage(argv[0]); return 1; }
            if (strcmp(argv[i], "socks5") == 0) {
                config.mode = MODE_SOCKS5;
            } else if (strcmp(argv[i], "forward_l") == 0) {
                config.mode = MODE_FORWARD_L;
            } else if (strcmp(argv[i], "forward_r") == 0) {
                config.mode = MODE_FORWARD_R;
            } else {
                fprintf(stderr, "Unknown mode: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (++i >= argc) { print_usage(argv[0]); return 1; }
            config.local_port = atoi(argv[i]);
        } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--remote") == 0) {
            if (++i >= argc) { print_usage(argv[0]); return 1; }
            remote_str = argv[i];
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--target") == 0) {
            if (++i >= argc) { print_usage(argv[0]); return 1; }
            target_str = argv[i];
        } else if (strcmp(argv[i], "-k") == 0 || strcmp(argv[i], "--psk") == 0) {
            if (++i >= argc) { print_usage(argv[0]); return 1; }
            psk_str = argv[i];
        } else if (strcmp(argv[i], "--odd") == 0) {
            g_is_odd_id_generator = 1;
        } else if (strcmp(argv[i], "--even") == 0) {
            g_is_odd_id_generator = 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (config.local_port <= 0) {
        fprintf(stderr, "Error: Local port (-p) is required and must be greater than 0.\n");
        return 1;
    }

    if (psk_str == NULL) {
        fprintf(stderr, "Error: Pre-shared key (-k) is required.\n");
        return 1;
    }

    /* Copy/pad PSK to 32 bytes */
    size_t psk_len = strlen(psk_str);
    if (psk_len >= PSK_LEN) {
        memcpy(config.psk, psk_str, PSK_LEN);
    } else {
        memcpy(config.psk, psk_str, psk_len);
        memset(config.psk + psk_len, 0, PSK_LEN - psk_len);
    }

    /* Parse remote address */
    if (remote_str) {
        char *colon = strchr(remote_str, ':');
        if (!colon) {
            fprintf(stderr, "Error: Remote address must be in IP:Port format (e.g. 127.0.0.1:9000).\n");
            return 1;
        }
        *colon = '\0';
        config.remote_ip = remote_str;
        config.remote_port = atoi(colon + 1);
    }

    /* Parse target address for static forwarding modes */
    if (config.mode == MODE_FORWARD_L || config.mode == MODE_FORWARD_R) {
        if (!target_str) {
            fprintf(stderr, "Error: Target address (-t) in IP:Port format is required for forwarding modes.\n");
            return 1;
        }
        char *colon = strchr(target_str, ':');
        if (!colon) {
            fprintf(stderr, "Error: Target address must be in IP:Port format (e.g. 127.0.0.1:80).\n");
            return 1;
        }
        *colon = '\0';
        config.target_ip = target_str;
        config.target_port = atoi(colon + 1);
    }

    printf("[Main] Initializing tunnel (Local Port: %d, Mode: %s, ID Generator: %s)...\n",
           config.local_port,
           config.mode == MODE_SOCKS5 ? "socks5" : (config.mode == MODE_FORWARD_L ? "forward_l" : "forward_r"),
           g_is_odd_id_generator ? "ODD" : "EVEN");

    bitun_osal_dns_init();

    if (tunnel_init(&g_tun, &config) < 0) {
        fprintf(stderr, "Error: Tunnel initialization failed.\n");
        bitun_osal_dns_deinit();
        return 1;
    }

    /* Block and run event loop */
    tunnel_run(&g_tun);

    bitun_osal_dns_deinit();
    tunnel_destroy(&g_tun);
    return 0;
}
