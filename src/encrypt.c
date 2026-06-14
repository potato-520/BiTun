#include "encrypt.h"
#include <string.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

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
    unsigned int mac_len = 0;
    HMAC(EVP_sha256(), key, key_len, data, data_len, mac_out, &mac_len);
    return (mac_len == 32) ? 0 : -1;
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

    uint8_t prk[32];
    /* HKDF-Extract: PRK = HMAC-SHA256(Salt, IKM = PSK) */
    unsigned int prk_len = 0;
    HMAC(EVP_sha256(), salt, 64, psk, psk_len, prk, &prk_len);
    if (prk_len != 32) return -1;

    /* HKDF-Expand: Session_Key = HMAC-SHA256(PRK, info || 0x01) */
    const char *info = "BiTun Ephemeral Key";
    size_t info_len = strlen(info);
    uint8_t info_buf[64];
    memcpy(info_buf, info, info_len);
    info_buf[info_len] = 0x01;

    unsigned int out_len = 0;
    HMAC(EVP_sha256(), prk, 32, info_buf, info_len + 1, key_out, &out_len);
    return (out_len == 32) ? 0 : -1;
}

/* ChaCha20-Poly1305 AEAD Encrypt */
int encrypt_chacha20_poly1305(const uint8_t *key, const uint8_t *nonce,
                              const uint8_t *plaintext, int plaintext_len,
                              uint8_t *ciphertext, uint8_t *tag) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int len = 0;
    int ciphertext_len = 0;

    if (EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL) != 1) goto err;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, AEAD_NONCE_LEN, NULL) != 1) goto err;
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) goto err;
    if (EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, plaintext_len) != 1) goto err;
    ciphertext_len = len;

    if (EVP_EncryptFinal_ex(ctx, ciphertext + len, &len) != 1) goto err;
    ciphertext_len += len;

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, AEAD_TAG_LEN, tag) != 1) goto err;

    EVP_CIPHER_CTX_free(ctx);
    return ciphertext_len;

err:
    EVP_CIPHER_CTX_free(ctx);
    return -1;
}

/* ChaCha20-Poly1305 AEAD Decrypt */
int decrypt_chacha20_poly1305(const uint8_t *key, const uint8_t *nonce,
                              const uint8_t *ciphertext, int ciphertext_len,
                              const uint8_t *tag, uint8_t *plaintext) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int len = 0;
    int plaintext_len = 0;
    int ret = 0;

    if (EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL) != 1) goto err;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, AEAD_NONCE_LEN, NULL) != 1) goto err;
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) goto err;
    if (EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ciphertext_len) != 1) goto err;
    plaintext_len = len;

    /* Set expected tag value */
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, AEAD_TAG_LEN, (void *)tag) != 1) goto err;

    /* EVP_DecryptFinal_ex will verify the tag and return >0 if successful */
    ret = EVP_DecryptFinal_ex(ctx, plaintext + len, &len);
    plaintext_len += len;

    EVP_CIPHER_CTX_free(ctx);
    return (ret > 0) ? plaintext_len : -1;

err:
    EVP_CIPHER_CTX_free(ctx);
    return -1;
}
