#ifndef BITUN_ENCRYPT_H
#define BITUN_ENCRYPT_H

#include <stdint.h>
#include <stddef.h>

#define AEAD_TAG_LEN 16
#define AEAD_NONCE_LEN 12
#define PSK_LEN 32
#define SESSION_KEY_LEN 32

/* Anti-replay sliding window structure */
typedef struct {
    uint64_t seq_max;
    uint64_t bitmap; // 64-bit sliding window bitmap
} anti_replay_window_t;

/* Initialize anti-replay window */
void anti_replay_init(anti_replay_window_t *win);

/* Check if sequence number is valid (not replayed/old) */
int anti_replay_check(anti_replay_window_t *win, uint64_t seq);

/* Update anti-replay window with a new validated sequence number */
void anti_replay_update(anti_replay_window_t *win, uint64_t seq);

/* HMAC-SHA256 helper */
int calculate_hmac(const uint8_t *key, size_t key_len,
                   const uint8_t *data, size_t data_len,
                   uint8_t *mac_out);

/* Session Key derivation using HKDF-SHA256 (Extract-and-Expand) */
int derive_session_key(const uint8_t *psk, size_t psk_len,
                       const uint8_t *r_init, size_t r_init_len,
                       const uint8_t *r_resp, size_t r_resp_len,
                       uint8_t *key_out);

/* ChaCha20-Poly1305 AEAD Encrypt */
int encrypt_chacha20_poly1305(const uint8_t *key, const uint8_t *nonce,
                              const uint8_t *plaintext, int plaintext_len,
                              uint8_t *ciphertext, uint8_t *tag);

/* ChaCha20-Poly1305 AEAD Decrypt */
int decrypt_chacha20_poly1305(const uint8_t *key, const uint8_t *nonce,
                              const uint8_t *ciphertext, int ciphertext_len,
                              const uint8_t *tag, uint8_t *plaintext);

#endif /* BITUN_ENCRYPT_H */
