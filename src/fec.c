#include "fec.h"
#include <string.h>

uint8_t gf_exp[512];
uint8_t gf_log[256];

void gf_init(void) {
    int val = 1;
    for (int i = 0; i < 256; i++) {
        gf_exp[i] = val;
        gf_exp[i + 255] = val;
        gf_log[val] = i;
        val <<= 1;
        if (val & 0x100) {
            val ^= 0x11d; // 285 (x^8 + x^4 + x^3 + x^2 + 1)
        }
    }
    gf_log[0] = 0; // undefined in math, set to 0 for safety
}

void gf_mul_add_buf(uint8_t *dst, const uint8_t *src, uint8_t coef, size_t len) {
    if (coef == 0) return;
    if (coef == 1) {
        for (size_t i = 0; i < len; i++) {
            dst[i] ^= src[i];
        }
        return;
    }
    int log_c = gf_log[coef];
    for (size_t i = 0; i < len; i++) {
        if (src[i] != 0) {
            dst[i] ^= gf_exp[log_c + gf_log[src[i]]];
        }
    }
}

int fec_encode(uint8_t **data_blocks, int n, int r, size_t len, uint8_t **parity_blocks) {
    if (n <= 0 || r <= 0) return 0;
    for (int i = 0; i < r; i++) {
        memset(parity_blocks[i], 0, len);
        for (int j = 0; j < n; j++) {
            uint8_t coef = gf_div(1, (i + n) ^ j);
            gf_mul_add_buf(parity_blocks[i], data_blocks[j], coef, len);
        }
    }
    return 0;
}

static int gf_invert_matrix(uint8_t *A, uint8_t *A_inv, int n) {
    memset(A_inv, 0, n * n);
    for (int i = 0; i < n; i++) {
        A_inv[i * n + i] = 1;
    }

    for (int i = 0; i < n; i++) {
        int pivot_row = i;
        while (pivot_row < n && A[pivot_row * n + i] == 0) {
            pivot_row++;
        }
        if (pivot_row == n) {
            return -1; // Singular matrix
        }

        if (pivot_row != i) {
            for (int j = 0; j < n; j++) {
                uint8_t tmp = A[i * n + j];
                A[i * n + j] = A[pivot_row * n + j];
                A[pivot_row * n + j] = tmp;

                tmp = A_inv[i * n + j];
                A_inv[i * n + j] = A_inv[pivot_row * n + j];
                A_inv[pivot_row * n + j] = tmp;
            }
        }

        uint8_t pivot = A[i * n + i];
        if (pivot != 1) {
            uint8_t inv_pivot = gf_div(1, pivot);
            for (int j = 0; j < n; j++) {
                A[i * n + j] = gf_mul(A[i * n + j], inv_pivot);
                A_inv[i * n + j] = gf_mul(A_inv[i * n + j], inv_pivot);
            }
        }

        for (int row = 0; row < n; row++) {
            if (row != i) {
                uint8_t factor = A[row * n + i];
                if (factor != 0) {
                    for (int j = 0; j < n; j++) {
                        A[row * n + j] ^= gf_mul(A[i * n + j], factor);
                        A_inv[row * n + j] ^= gf_mul(A_inv[i * n + j], factor);
                    }
                }
            }
        }
    }
    return 0;
}

int fec_decode(uint8_t **received_blocks, int *received_indices, int num_received, int n, int r, size_t len, uint8_t **recovered_data) {
    (void)r;
    if (num_received < n) return -1;

    uint8_t A[16 * 16];
    uint8_t A_inv[16 * 16];
    if (n > 16) return -1;

    for (int i = 0; i < n; i++) {
        int idx = received_indices[i];
        if (idx < n) {
            for (int j = 0; j < n; j++) {
                A[i * n + j] = (j == idx) ? 1 : 0;
            }
        } else {
            for (int j = 0; j < n; j++) {
                A[i * n + j] = gf_div(1, idx ^ j);
            }
        }
    }

    if (gf_invert_matrix(A, A_inv, n) < 0) {
        return -1;
    }

    for (int j = 0; j < n; j++) {
        int found_idx = -1;
        for (int i = 0; i < n; i++) {
            if (received_indices[i] == j) {
                found_idx = i;
                break;
            }
        }
        if (found_idx != -1) {
            memcpy(recovered_data[j], received_blocks[found_idx], len);
        } else {
            memset(recovered_data[j], 0, len);
            for (int i = 0; i < n; i++) {
                uint8_t coef = A_inv[j * n + i];
                gf_mul_add_buf(recovered_data[j], received_blocks[i], coef, len);
            }
        }
    }
    return 0;
}
