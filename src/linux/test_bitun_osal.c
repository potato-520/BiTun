#include "bitun_osal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static void test_time_and_random(void) {
    printf("[Test] Running time and random test...\n");
    uint64_t t1 = bitun_osal_time_get_ms();
    bitun_osal_thread_sleep_ms(100);
    uint64_t t2 = bitun_osal_time_get_ms();
    assert(t2 >= t1 + 90);
    printf("[Test] Monotonic time check passed: elapsed %lu ms\n", (unsigned long)(t2 - t1));

    uint64_t r1 = bitun_osal_time_get_real_ms();
    assert(r1 > 0);
    printf("[Test] Real calendar time: %lu ms\n", (unsigned long)r1);

    uint32_t rand_val = bitun_osal_random_u32();
    printf("[Test] Random U32: 0x%08x\n", rand_val);

    uint8_t rand_bytes[16];
    bitun_osal_random_bytes(rand_bytes, sizeof(rand_bytes));
    int all_zeros = 1;
    for (size_t i = 0; i < sizeof(rand_bytes); i++) {
        if (rand_bytes[i] != 0) {
            all_zeros = 0;
            break;
        }
    }
    assert(!all_zeros);
    printf("[Test] Random bytes test passed.\n");
}

static void test_crypto(void) {
    printf("[Test] Running crypto (HMAC, HKDF, ChaCha20-Poly1305) test...\n");
    uint8_t key[32] = "my_secret_key_12345678901234567";
    uint8_t data[] = "Hello World! This is OSAL Cryptography Test.";
    uint8_t mac[32];
    int ret = bitun_osal_crypto_hmac_sha256(key, sizeof(key), data, sizeof(data), mac);
    assert(ret == 0);
    printf("[Test] HMAC-SHA256 calculated successfully.\n");

    uint8_t salt[16] = "crypto_salt";
    uint8_t okm[32];
    ret = bitun_osal_crypto_hkdf_sha256(salt, sizeof(salt), key, sizeof(key), (uint8_t *)"info", 4, okm, sizeof(okm));
    assert(ret == 0);
    printf("[Test] HKDF-SHA256 derived successfully.\n");

    uint8_t nonce[12] = "nonce_12byte";
    uint8_t plaintext[] = "Confidential Payload!";
    size_t pt_len = sizeof(plaintext);
    uint8_t ciphertext[64];
    uint8_t tag[16];
    ret = bitun_osal_crypto_chacha20_poly1305_encrypt(okm, nonce, plaintext, pt_len, ciphertext, tag);
    assert(ret > 0);
    printf("[Test] ChaCha20-Poly1305 Encrypt passed. Ciphertext len: %d\n", ret);

    uint8_t decrypted[64];
    ret = bitun_osal_crypto_chacha20_poly1305_decrypt(okm, nonce, ciphertext, pt_len, tag, decrypted);
    assert(ret > 0);
    assert(memcmp(plaintext, decrypted, pt_len) == 0);
    printf("[Test] ChaCha20-Poly1305 Decrypt passed.\n");

    // In-place decryption test
    uint8_t inplace_buf[64];
    memcpy(inplace_buf, ciphertext, pt_len);
    ret = bitun_osal_crypto_chacha20_poly1305_decrypt(okm, nonce, inplace_buf, pt_len, tag, inplace_buf);
    assert(ret > 0);
    assert(memcmp(plaintext, inplace_buf, pt_len) == 0);
    printf("[Test] ChaCha20-Poly1305 In-place Decrypt passed.\n");
}

static void *test_thread_func(void *arg) {
    bitun_osal_mutex_t *mutex = (bitun_osal_mutex_t *)arg;
    bitun_osal_mutex_lock(mutex);
    printf("[Test] Thread locked mutex successfully.\n");
    bitun_osal_thread_sleep_ms(50);
    bitun_osal_mutex_unlock(mutex);
    return NULL;
}

static void test_threading_and_mutex(void) {
    printf("[Test] Running threading and mutex test...\n");
    bitun_osal_mutex_t *mutex = NULL;
    int ret = bitun_osal_mutex_create(&mutex);
    assert(ret == 0 && mutex != NULL);

    bitun_osal_mutex_lock(mutex);
    bitun_osal_thread_t *thread = NULL;
    ret = bitun_osal_thread_create(&thread, "test_thread", 4096, 1, test_thread_func, mutex);
    assert(ret == 0 && thread != NULL);

    printf("[Test] Main thread sleeping for a bit while holding mutex...\n");
    bitun_osal_thread_sleep_ms(50);
    bitun_osal_mutex_unlock(mutex);

    ret = bitun_osal_thread_detach(thread);
    assert(ret == 0);

    bitun_osal_thread_sleep_ms(100); // Wait for thread to finish
    bitun_osal_mutex_destroy(mutex);
    printf("[Test] Threading and mutex test passed.\n");
}

static void test_queue(void) {
    printf("[Test] Running eventfd queue test...\n");
    typedef struct {
        int id;
        char name[16];
    } msg_t;

    bitun_osal_queue_t *q = bitun_osal_queue_create(sizeof(msg_t), 4);
    assert(q != NULL);

    msg_t m1 = {1, "Alice"};
    msg_t m2 = {2, "Bob"};

    assert(bitun_osal_queue_push(q, &m1) == 0);
    assert(bitun_osal_queue_push(q, &m2) == 0);

    bitun_socket_t fd = bitun_osal_queue_get_read_fd(q);
    assert(fd >= 0);

    // Wait on poll set to test eventfd trigger
    bitun_osal_poll_set_t *ps = bitun_osal_poll_create();
    assert(ps != NULL);
    bitun_osal_poll_add(ps, fd, BITUN_POLL_IN);

    bitun_osal_event_t ev;
    int nfds = bitun_osal_poll_wait(ps, 50, &ev, 1);
    assert(nfds == 1);
    assert(ev.fd == fd);
    assert(ev.events & BITUN_POLL_IN);

    // Pop and verify
    msg_t out;
    assert(bitun_osal_queue_pop(q, &out) == 0);
    assert(out.id == 1 && strcmp(out.name, "Alice") == 0);

    bitun_osal_queue_clear_wakeup(q);

    assert(bitun_osal_queue_pop(q, &out) == 0);
    assert(out.id == 2 && strcmp(out.name, "Bob") == 0);

    bitun_osal_queue_destroy(q);
    bitun_osal_poll_destroy(ps);
    printf("[Test] Eventfd queue test passed.\n");
}

static void test_async_dns(void) {
    printf("[Test] Running async DNS test...\n");
    int ret = bitun_osal_dns_init();
    assert(ret == 0);

    bitun_osal_queue_t *res_q = bitun_osal_queue_create(sizeof(bitun_osal_dns_result_t), 4);
    assert(res_q != NULL);

    ret = bitun_osal_dns_resolve_async("localhost", 101, res_q);
    assert(ret == 0);

    // Wait on poll
    bitun_osal_poll_set_t *ps = bitun_osal_poll_create();
    bitun_osal_poll_add(ps, bitun_osal_queue_get_read_fd(res_q), BITUN_POLL_IN);

    bitun_osal_event_t ev;
    int nfds = bitun_osal_poll_wait(ps, 2000, &ev, 1);
    assert(nfds == 1);

    bitun_osal_queue_clear_wakeup(res_q);

    bitun_osal_dns_result_t res;
    ret = bitun_osal_queue_pop(res_q, &res);
    assert(ret == 0);
    assert(res.channel_id == 101);
    assert(res.success == 1);
    
    if (res.resolved_addr) {
        struct sockaddr_in *addr = (struct sockaddr_in *)res.resolved_addr;
        printf("[Test] Resolved localhost to: %s\n", inet_ntoa(addr->sin_addr));
        free(res.resolved_addr);
    } else {
        printf("[Test] Resolved IPv4: %d.%d.%d.%d\n", res.resolved_ipv4[0], res.resolved_ipv4[1], res.resolved_ipv4[2], res.resolved_ipv4[3]);
    }

    bitun_osal_dns_deinit();
    bitun_osal_queue_destroy(res_q);
    bitun_osal_poll_destroy(ps);
    printf("[Test] Async DNS test passed.\n");
}

int main(void) {
    printf("==========================================\n");
    printf("Starting BiTun OSAL Unit Tests...\n");
    printf("==========================================\n");
    
    test_time_and_random();
    test_crypto();
    test_threading_and_mutex();
    test_queue();
    test_async_dns();

    printf("==========================================\n");
    printf("All BiTun OSAL Unit Tests Passed!\n");
    printf("==========================================\n");
    return 0;
}
