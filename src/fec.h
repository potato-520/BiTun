#ifndef BITUN_FEC_H
#define BITUN_FEC_H

#include <stdint.h>
#include <stddef.h>

#define FEC_MAX_N 16
#define FEC_MAX_R 8
#define FEC_BLOCK_SIZE 1408
#define FEC_MAX_GROUPS 3

typedef struct {
    uint16_t group_id;
    uint8_t index;
    uint8_t n;
    uint8_t r;
    uint8_t reserved;
} __attribute__((packed)) fec_header_t;

typedef struct {
    uint16_t current_group_id;
    int data_packet_count;
    uint8_t n;
    uint8_t r;
    uint64_t last_packet_time;
    
    uint16_t data_lengths[FEC_MAX_N];
    uint8_t data_blocks[FEC_MAX_N][FEC_BLOCK_SIZE];
    uint8_t parity_blocks[FEC_MAX_R][FEC_BLOCK_SIZE];
} fec_encoder_t;

typedef struct {
    uint16_t group_id;
    uint8_t n;
    uint8_t r;
    int received_count;
    uint64_t last_active_time;
    
    int received_indices[FEC_MAX_N + FEC_MAX_R];
    uint8_t has_index[FEC_MAX_N + FEC_MAX_R];
    int decoded;
    
    uint8_t received_blocks[FEC_MAX_N + FEC_MAX_R][FEC_BLOCK_SIZE];
} fec_group_t;

typedef struct {
    fec_group_t groups[FEC_MAX_GROUPS];
    uint16_t last_processed_group_id;
    
    uint8_t recovered_data[FEC_MAX_N][FEC_BLOCK_SIZE];
} fec_decoder_t;

extern uint8_t gf_exp[512];
extern uint8_t gf_log[256];

void gf_init(void);

static inline uint8_t gf_add(uint8_t a, uint8_t b) {
    return a ^ b;
}

static inline uint8_t gf_sub(uint8_t a, uint8_t b) {
    return a ^ b;
}

static inline uint8_t gf_mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    return gf_exp[gf_log[a] + gf_log[b]];
}

static inline uint8_t gf_div(uint8_t a, uint8_t b) {
    if (a == 0) return 0;
    if (b == 0) return 0; // Avoid division by zero
    int diff = gf_log[a] - gf_log[b];
    if (diff < 0) diff += 255;
    return gf_exp[diff];
}

void gf_mul_add_buf(uint8_t *dst, const uint8_t *src, uint8_t coef, size_t len);

int fec_encode(uint8_t **data_blocks, int n, int r, size_t len, uint8_t **parity_blocks);
int fec_decode(uint8_t **received_blocks, int *received_indices, int num_received, int n, int r, size_t len, uint8_t **recovered_data);

#endif // BITUN_FEC_H
