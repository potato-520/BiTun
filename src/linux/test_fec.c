#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../fec.h"

int main() {
    srand(time(NULL));
    gf_init();
    printf("GF(256) tables initialized.\n");

    int n = 10;
    int r = 4;
    size_t len = 100;

    uint8_t *data_blocks[16];
    uint8_t *orig_data[16];
    for (int i = 0; i < n; i++) {
        data_blocks[i] = malloc(len);
        orig_data[i] = malloc(len);
        for (size_t j = 0; j < len; j++) {
            data_blocks[i][j] = rand() % 256;
        }
        memcpy(orig_data[i], data_blocks[i], len);
    }

    uint8_t *parity_blocks[8];
    for (int i = 0; i < r; i++) {
        parity_blocks[i] = malloc(len);
    }

    fec_encode(data_blocks, n, r, len, parity_blocks);
    printf("FEC systematic encoding completed.\n");

    uint8_t *received_blocks[16];
    int received_indices[16];
    int num_received = 0;

    int data_rec_indices[] = {0, 1, 3, 4, 6, 8};
    for (int i = 0; i < 6; i++) {
        int idx = data_rec_indices[i];
        received_blocks[num_received] = malloc(len);
        memcpy(received_blocks[num_received], orig_data[idx], len);
        received_indices[num_received] = idx;
        num_received++;
    }

    int parity_rec_indices[] = {10, 11, 12, 13};
    for (int i = 0; i < 4; i++) {
        int idx = parity_rec_indices[i];
        received_blocks[num_received] = malloc(len);
        memcpy(received_blocks[num_received], parity_blocks[idx - n], len);
        received_indices[num_received] = idx;
        num_received++;
    }

    uint8_t *recovered_data[16];
    for (int i = 0; i < n; i++) {
        recovered_data[i] = malloc(len);
    }

    int ret = fec_decode(received_blocks, received_indices, num_received, n, r, len, recovered_data);
    printf("FEC decoding result: %d\n", ret);

    if (ret == 0) {
        int match = 1;
        for (int i = 0; i < n; i++) {
            if (memcmp(orig_data[i], recovered_data[i], len) != 0) {
                printf("Mismatch at data block %d!\n", i);
                match = 0;
            }
        }
        if (match) {
            printf("SUCCESS: All recovered blocks match the original data!\n");
        }
    } else {
        printf("FAILED decoding!\n");
    }

    for (int i = 0; i < n; i++) {
        free(data_blocks[i]);
        free(orig_data[i]);
        free(recovered_data[i]);
    }
    for (int i = 0; i < r; i++) {
        free(parity_blocks[i]);
    }
    for (int i = 0; i < num_received; i++) {
        free(received_blocks[i]);
    }

    return ret;
}
