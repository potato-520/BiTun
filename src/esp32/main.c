#include "bitun_osal.h"
#include <stdio.h>
#include <stdlib.h>

void bitun_esp32_init(void) {
    printf("[BiTun ESP32] Initializing OSAL DNS and subsystem...\n");
    // Invoke OSAL init to ensure linking compatibility
    bitun_osal_dns_init();
}

/* --- OSAL Socket Stub Implementations --- */
bitun_socket_t bitun_osal_socket_create(int domain, int type, int protocol) {
    (void)domain; (void)type; (void)protocol;
    return BITUN_INVALID_SOCKET;
}

int bitun_osal_socket_close(bitun_socket_t fd) {
    (void)fd;
    return -1;
}

int bitun_osal_socket_bind(bitun_socket_t fd, const struct sockaddr *addr, bitun_socklen_t addrlen) {
    (void)fd; (void)addr; (void)addrlen;
    return -1;
}

int bitun_osal_socket_listen(bitun_socket_t fd, int backlog) {
    (void)fd; (void)backlog;
    return -1;
}

bitun_socket_t bitun_osal_socket_accept(bitun_socket_t fd, struct sockaddr *addr, bitun_socklen_t *addrlen) {
    (void)fd; (void)addr; (void)addrlen;
    return BITUN_INVALID_SOCKET;
}

int bitun_osal_socket_connect(bitun_socket_t fd, const struct sockaddr *addr, bitun_socklen_t addrlen) {
    (void)fd; (void)addr; (void)addrlen;
    return -1;
}

int bitun_osal_socket_send(bitun_socket_t fd, const void *buf, size_t len, int flags) {
    (void)fd; (void)buf; (void)len; (void)flags;
    return -1;
}

int bitun_osal_socket_recv(bitun_socket_t fd, void *buf, size_t len, int flags) {
    (void)fd; (void)buf; (void)len; (void)flags;
    return -1;
}

int bitun_osal_socket_sendto(bitun_socket_t fd, const void *buf, size_t len, int flags,
                             const struct sockaddr *dest_addr, bitun_socklen_t addrlen) {
    (void)fd; (void)buf; (void)len; (void)flags; (void)dest_addr; (void)addrlen;
    return -1;
}

int bitun_osal_socket_recvfrom(bitun_socket_t fd, void *buf, size_t len, int flags,
                               struct sockaddr *src_addr, bitun_socklen_t *addrlen) {
    (void)fd; (void)buf; (void)len; (void)flags; (void)src_addr; (void)addrlen;
    return -1;
}

int bitun_osal_socket_set_nonblocking(bitun_socket_t fd) {
    (void)fd;
    return -1;
}

int bitun_osal_socket_set_reuseaddr(bitun_socket_t fd) {
    (void)fd;
    return -1;
}

/* --- OSAL Multiplexing (Poll) Stub Implementations --- */
bitun_osal_poll_set_t *bitun_osal_poll_create(void) {
    return NULL;
}

void bitun_osal_poll_destroy(bitun_osal_poll_set_t *set) {
    (void)set;
}

int bitun_osal_poll_add(bitun_osal_poll_set_t *set, bitun_socket_t fd, uint32_t events) {
    (void)set; (void)fd; (void)events;
    return -1;
}

int bitun_osal_poll_mod(bitun_osal_poll_set_t *set, bitun_socket_t fd, uint32_t events) {
    (void)set; (void)fd; (void)events;
    return -1;
}

int bitun_osal_poll_del(bitun_osal_poll_set_t *set, bitun_socket_t fd) {
    (void)set; (void)fd;
    return -1;
}

int bitun_osal_poll_wait(bitun_osal_poll_set_t *set, int timeout_ms, 
                         bitun_osal_event_t *events_out, int max_events) {
    (void)set; (void)timeout_ms; (void)events_out; (void)max_events;
    return -1;
}

/* --- OSAL Thread and Mutex Stub Implementations --- */
int bitun_osal_thread_create(bitun_osal_thread_t **thread_out, const char *name, 
                             uint32_t stack_size, uint32_t priority,
                             bitun_osal_thread_entry_t entry, void *arg) {
    (void)thread_out; (void)name; (void)stack_size; (void)priority; (void)entry; (void)arg;
    return -1;
}

int bitun_osal_thread_detach(bitun_osal_thread_t *thread) {
    (void)thread;
    return -1;
}

void bitun_osal_thread_sleep_ms(uint32_t ms) {
    (void)ms;
}

int bitun_osal_mutex_create(bitun_osal_mutex_t **mutex_out) {
    (void)mutex_out;
    return -1;
}

int bitun_osal_mutex_lock(bitun_osal_mutex_t *mutex) {
    (void)mutex;
    return -1;
}

int bitun_osal_mutex_unlock(bitun_osal_mutex_t *mutex) {
    (void)mutex;
    return -1;
}

int bitun_osal_mutex_destroy(bitun_osal_mutex_t *mutex) {
    (void)mutex;
    return -1;
}

/* --- OSAL Queue Stub Implementations --- */
bitun_osal_queue_t *bitun_osal_queue_create(size_t item_size, size_t capacity) {
    (void)item_size; (void)capacity;
    return NULL;
}

void bitun_osal_queue_destroy(bitun_osal_queue_t *q) {
    (void)q;
}

int bitun_osal_queue_push(bitun_osal_queue_t *q, const void *item) {
    (void)q; (void)item;
    return -1;
}

int bitun_osal_queue_pop(bitun_osal_queue_t *q, void *item_out) {
    (void)q; (void)item_out;
    return -1;
}

bitun_socket_t bitun_osal_queue_get_read_fd(bitun_osal_queue_t *q) {
    (void)q;
    return BITUN_INVALID_SOCKET;
}

void bitun_osal_queue_clear_wakeup(bitun_osal_queue_t *q) {
    (void)q;
}

/* --- OSAL DNS Stub Implementations --- */
int bitun_osal_dns_init(void) {
    return 0;
}

void bitun_osal_dns_deinit(void) {
}

int bitun_osal_dns_resolve_async(const char *domain, uint32_t channel_id, 
                                 bitun_osal_queue_t *result_queue) {
    (void)domain; (void)channel_id; (void)result_queue;
    return -1;
}

/* --- OSAL Crypto Stub Implementations --- */
int bitun_osal_crypto_hmac_sha256(const uint8_t *key, size_t key_len,
                                  const uint8_t *data, size_t data_len,
                                  uint8_t *mac_out) {
    (void)key; (void)key_len; (void)data; (void)data_len; (void)mac_out;
    return -1;
}

int bitun_osal_crypto_hkdf_sha256(const uint8_t *salt, size_t salt_len,
                                  const uint8_t *ikm, size_t ikm_len,
                                  const uint8_t *info, size_t info_len,
                                  uint8_t *okm_out, size_t okm_len) {
    (void)salt; (void)salt_len; (void)ikm; (void)ikm_len; (void)info; (void)info_len; (void)okm_out; (void)okm_len;
    return -1;
}

int bitun_osal_crypto_chacha20_poly1305_encrypt(const uint8_t *key, const uint8_t *nonce,
                                                const uint8_t *plaintext, size_t plaintext_len,
                                                uint8_t *ciphertext_out, uint8_t *tag_out) {
    (void)key; (void)nonce; (void)plaintext; (void)plaintext_len; (void)ciphertext_out; (void)tag_out;
    return -1;
}

int bitun_osal_crypto_chacha20_poly1305_decrypt(const uint8_t *key, const uint8_t *nonce,
                                                const uint8_t *ciphertext, size_t ciphertext_len,
                                                const uint8_t *tag, uint8_t *plaintext_out) {
    (void)key; (void)nonce; (void)ciphertext; (void)ciphertext_len; (void)tag; (void)plaintext_out;
    return -1;
}

/* --- OSAL Time & Random Stub Implementations --- */
uint64_t bitun_osal_time_get_ms(void) {
    return 0;
}

uint64_t bitun_osal_time_get_real_ms(void) {
    return 0;
}

uint32_t bitun_osal_random_u32(void) {
    return 0;
}

void bitun_osal_random_bytes(uint8_t *buf, size_t len) {
    (void)buf; (void)len;
}
