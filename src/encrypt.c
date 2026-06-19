#include "encrypt.h"
#include "bitun_osal.h"
#include <string.h>

/* Initialize anti-replay window */
void anti_replay_init(anti_replay_window_t *win) {
    win->seq_max = 0;
    win->bitmap = 0;
}

/* Check if sequence number is valid (not replayed/old) */
int anti_replay_check(anti_replay_window_t *win, uint64_t seq) {
    if (seq == 0) return 0; // 0 is invalid sequence number
    if (seq > win->seq_max) {
        return 1; // Ahead of window, valid
    }
    uint64_t offset = win->seq_max - seq;
    if (offset >= 64) {
        return 0; // Behind window (too old)
    }
    if (win->bitmap & ((uint64_t)1 << offset)) {
        return 0; // Duplicate packet (already received)
    }
    return 1; // Valid
}

/* Update anti-replay window with a new validated sequence number */
void anti_replay_update(anti_replay_window_t *win, uint64_t seq) {
    if (seq > win->seq_max) {
        uint64_t shift = seq - win->seq_max;
        if (shift < 64) {
            win->bitmap <<= shift;
        } else {
            win->bitmap = 0;
        }
        win->bitmap |= (uint64_t)1;
        win->seq_max = seq;
    } else {
        uint64_t offset = win->seq_max - seq;
        if (offset < 64) {
            win->bitmap |= ((uint64_t)1 << offset);
        }
    }
}

/* HMAC-SHA256 helper */
int calculate_hmac(const uint8_t *key, size_t key_len,
                   const uint8_t *data, size_t data_len,
                   uint8_t *mac_out) {
    return bitun_osal_crypto_hmac_sha256(key, key_len, data, data_len, mac_out);
}

/* Session Key derivation using HKDF-SHA256 (Extract-and-Expand) */
int derive_session_key(const uint8_t *psk, size_t psk_len,
                       const uint8_t *r_init, size_t r_init_len,
                       const uint8_t *r_resp, size_t r_resp_len,
                       uint8_t *key_out) {
    (void)r_init_len;
    (void)r_resp_len;

    uint8_t salt[64];
    /* Sort R_init and R_resp using memcmp to ensure both ends construct the identical Salt */
    if (memcmp(r_init, r_resp, 32) > 0) {
        memcpy(salt, r_init, 32);
        memcpy(salt + 32, r_resp, 32);
    } else {
        memcpy(salt, r_resp, 32);
        memcpy(salt + 32, r_init, 32);
    }

    const char *info = "BiTun Ephemeral Key";
    return bitun_osal_crypto_hkdf_sha256(salt, 64, psk, psk_len, (const uint8_t *)info, strlen(info), key_out, 32);
}

/* ChaCha20-Poly1305 AEAD Encrypt */
int encrypt_chacha20_poly1305(const uint8_t *key, const uint8_t *nonce,
                              const uint8_t *plaintext, int plaintext_len,
                              uint8_t *ciphertext, uint8_t *tag) {
    return bitun_osal_crypto_chacha20_poly1305_encrypt(key, nonce, plaintext, plaintext_len, ciphertext, tag);
}

/* ChaCha20-Poly1305 AEAD Decrypt */
int decrypt_chacha20_poly1305(const uint8_t *key, const uint8_t *nonce,
                              const uint8_t *ciphertext, int ciphertext_len,
                              const uint8_t *tag, uint8_t *plaintext) {
    return bitun_osal_crypto_chacha20_poly1305_decrypt(key, nonce, ciphertext, ciphertext_len, tag, plaintext);
}
