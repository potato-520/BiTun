/**
 * @file bitun_osal.c
 * @brief OSAL Linux implementation for BiTun
 */

#define OPENSSL_API_COMPAT 0x10101000L
#include "bitun_osal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>

/* ========================================================================== */
/* 1. 套接字网络抽象 API                                                      */
/* ========================================================================== */

bitun_socket_t bitun_osal_socket_create(int domain, int type, int protocol) {
    return socket(domain, type, protocol);
}

int bitun_osal_socket_close(bitun_socket_t fd) {
    if (fd >= 0) {
        return close(fd);
    }
    return 0;
}

int bitun_osal_socket_bind(bitun_socket_t fd, const struct sockaddr *addr, bitun_socklen_t addrlen) {
    return bind(fd, addr, addrlen);
}

int bitun_osal_socket_listen(bitun_socket_t fd, int backlog) {
    return listen(fd, backlog);
}

bitun_socket_t bitun_osal_socket_accept(bitun_socket_t fd, struct sockaddr *addr, bitun_socklen_t *addrlen) {
    return accept(fd, addr, addrlen);
}

int bitun_osal_socket_connect(bitun_socket_t fd, const struct sockaddr *addr, bitun_socklen_t addrlen) {
    return connect(fd, addr, addrlen);
}

int bitun_osal_socket_send(bitun_socket_t fd, const void *buf, size_t len, int flags) {
    return send(fd, buf, len, flags);
}

int bitun_osal_socket_recv(bitun_socket_t fd, void *buf, size_t len, int flags) {
    return recv(fd, buf, len, flags);
}

int bitun_osal_socket_sendto(bitun_socket_t fd, const void *buf, size_t len, int flags,
                             const struct sockaddr *dest_addr, bitun_socklen_t addrlen) {
    return sendto(fd, buf, len, flags, dest_addr, addrlen);
}

int bitun_osal_socket_recvfrom(bitun_socket_t fd, void *buf, size_t len, int flags,
                               struct sockaddr *src_addr, bitun_socklen_t *addrlen) {
    return recvfrom(fd, buf, len, flags, src_addr, addrlen);
}

int bitun_osal_socket_set_nonblocking(bitun_socket_t fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int bitun_osal_socket_set_reuseaddr(bitun_socket_t fd) {
    int reuse = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
}

/* ========================================================================== */
/* 2. 多路复用事件监听接口 (Epoll/Poll 统一封装)                                  */
/* ========================================================================== */

struct bitun_osal_poll_set {
    int epoll_fd;
};

bitun_osal_poll_set_t *bitun_osal_poll_create(void) {
    bitun_osal_poll_set_t *set = malloc(sizeof(*set));
    if (!set) return NULL;
    set->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (set->epoll_fd < 0) {
        free(set);
        return NULL;
    }
    return set;
}

void bitun_osal_poll_destroy(bitun_osal_poll_set_t *set) {
    if (set) {
        if (set->epoll_fd >= 0) {
            close(set->epoll_fd);
        }
        free(set);
    }
}

int bitun_osal_poll_add(bitun_osal_poll_set_t *set, bitun_socket_t fd, uint32_t events) {
    if (!set || set->epoll_fd < 0) return -1;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.data.fd = fd;
    if (events & BITUN_POLL_IN) ev.events |= EPOLLIN;
    if (events & BITUN_POLL_OUT) ev.events |= EPOLLOUT;
    if (events & BITUN_POLL_ERR) ev.events |= EPOLLERR;
    return epoll_ctl(set->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

int bitun_osal_poll_mod(bitun_osal_poll_set_t *set, bitun_socket_t fd, uint32_t events) {
    if (!set || set->epoll_fd < 0) return -1;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.data.fd = fd;
    if (events & BITUN_POLL_IN) ev.events |= EPOLLIN;
    if (events & BITUN_POLL_OUT) ev.events |= EPOLLOUT;
    if (events & BITUN_POLL_ERR) ev.events |= EPOLLERR;
    return epoll_ctl(set->epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

int bitun_osal_poll_del(bitun_osal_poll_set_t *set, bitun_socket_t fd) {
    if (!set || set->epoll_fd < 0) return -1;
    return epoll_ctl(set->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
}

int bitun_osal_poll_wait(bitun_osal_poll_set_t *set, int timeout_ms, 
                         bitun_osal_event_t *events_out, int max_events) {
    if (!set || set->epoll_fd < 0 || max_events <= 0) return -1;
    
    // Allocate space on stack for events
    struct epoll_event evs[max_events];
    int nfds = epoll_wait(set->epoll_fd, evs, max_events, timeout_ms);
    if (nfds < 0) {
        return -1;
    }
    
    for (int i = 0; i < nfds; i++) {
        events_out[i].fd = evs[i].data.fd;
        events_out[i].events = 0;
        if (evs[i].events & EPOLLIN) events_out[i].events |= BITUN_POLL_IN;
        if (evs[i].events & EPOLLOUT) events_out[i].events |= BITUN_POLL_OUT;
        if (evs[i].events & (EPOLLERR | EPOLLHUP)) events_out[i].events |= BITUN_POLL_ERR;
    }
    
    return nfds;
}

/* ========================================================================== */
/* 3. 线程与同步互斥锁接口                                                     */
/* ========================================================================== */

struct bitun_osal_thread {
    pthread_t thread;
};

int bitun_osal_thread_create(bitun_osal_thread_t **thread_out, const char *name, 
                             uint32_t stack_size, uint32_t priority,
                             bitun_osal_thread_entry_t entry, void *arg) {
    (void)name;
    (void)stack_size;
    (void)priority;
    
    bitun_osal_thread_t *t = malloc(sizeof(*t));
    if (!t) return -1;
    
    int ret = pthread_create(&t->thread, NULL, entry, arg);
    if (ret != 0) {
        free(t);
        return -1;
    }
    
    *thread_out = t;
    return 0;
}

int bitun_osal_thread_detach(bitun_osal_thread_t *thread) {
    if (!thread) return -1;
    int ret = pthread_detach(thread->thread);
    free(thread);
    return (ret == 0) ? 0 : -1;
}

void bitun_osal_thread_sleep_ms(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

struct bitun_osal_mutex {
    pthread_mutex_t mtx;
};

int bitun_osal_mutex_create(bitun_osal_mutex_t **mutex_out) {
    bitun_osal_mutex_t *m = malloc(sizeof(*m));
    if (!m) return -1;
    
    if (pthread_mutex_init(&m->mtx, NULL) != 0) {
        free(m);
        return -1;
    }
    
    *mutex_out = m;
    return 0;
}

int bitun_osal_mutex_lock(bitun_osal_mutex_t *mutex) {
    if (!mutex) return -1;
    return (pthread_mutex_lock(&mutex->mtx) == 0) ? 0 : -1;
}

int bitun_osal_mutex_unlock(bitun_osal_mutex_t *mutex) {
    if (!mutex) return -1;
    return (pthread_mutex_unlock(&mutex->mtx) == 0) ? 0 : -1;
}

int bitun_osal_mutex_destroy(bitun_osal_mutex_t *mutex) {
    if (!mutex) return -1;
    pthread_mutex_destroy(&mutex->mtx);
    free(mutex);
    return 0;
}

/* ========================================================================== */
/* 4. eventfd 跨线程/进程通信队列                                               */
/* ========================================================================== */

struct bitun_osal_queue {
    int ev_fd;
    uint8_t *buffer;
    size_t item_size;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    pthread_mutex_t lock;
};

bitun_osal_queue_t *bitun_osal_queue_create(size_t item_size, size_t capacity) {
    bitun_osal_queue_t *q = malloc(sizeof(*q));
    if (!q) return NULL;
    
    q->ev_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (q->ev_fd < 0) {
        free(q);
        return NULL;
    }
    
    q->buffer = malloc(item_size * capacity);
    if (!q->buffer) {
        close(q->ev_fd);
        free(q);
        return NULL;
    }
    
    q->item_size = item_size;
    q->capacity = capacity;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    
    if (pthread_mutex_init(&q->lock, NULL) != 0) {
        free(q->buffer);
        close(q->ev_fd);
        free(q);
        return NULL;
    }
    
    return q;
}

void bitun_osal_queue_destroy(bitun_osal_queue_t *q) {
    if (q) {
        if (q->ev_fd >= 0) {
            close(q->ev_fd);
        }
        free(q->buffer);
        pthread_mutex_destroy(&q->lock);
        free(q);
    }
}

int bitun_osal_queue_push(bitun_osal_queue_t *q, const void *item) {
    if (!q || !item) return -1;
    
    pthread_mutex_lock(&q->lock);
    if (q->count >= q->capacity) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    
    memcpy(q->buffer + (q->tail * q->item_size), item, q->item_size);
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    pthread_mutex_unlock(&q->lock);
    
    // Write 8-byte value 1 to eventfd
    uint64_t val = 1;
    ssize_t n = write(q->ev_fd, &val, sizeof(val));
    (void)n;
    
    return 0;
}

int bitun_osal_queue_pop(bitun_osal_queue_t *q, void *item_out) {
    if (!q || !item_out) return -1;
    
    pthread_mutex_lock(&q->lock);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    
    memcpy(item_out, q->buffer + (q->head * q->item_size), q->item_size);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_mutex_unlock(&q->lock);
    
    return 0;
}

bitun_socket_t bitun_osal_queue_get_read_fd(bitun_osal_queue_t *q) {
    return q ? q->ev_fd : -1;
}

void bitun_osal_queue_clear_wakeup(bitun_osal_queue_t *q) {
    if (q && q->ev_fd >= 0) {
        uint64_t val = 0;
        ssize_t n = read(q->ev_fd, &val, sizeof(val));
        (void)n;
    }
}

/* ========================================================================== */
/* 5. 全局异步 DNS 解析系统                                                    */
/* ========================================================================== */

typedef struct dns_task {
    char domain[256];
    uint32_t channel_id;
    bitun_osal_queue_t *result_queue;
    struct dns_task *next;
} dns_task_t;

static pthread_t dns_thread;
static pthread_mutex_t dns_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dns_cond = PTHREAD_COND_INITIALIZER;
static dns_task_t *dns_queue_head = NULL;
static dns_task_t *dns_queue_tail = NULL;
static int dns_thread_running = 0;
static int dns_should_stop = 0;

static void *dns_worker_thread(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&dns_lock);
        while (dns_queue_head == NULL && !dns_should_stop) {
            pthread_cond_wait(&dns_cond, &dns_lock);
        }
        if (dns_should_stop) {
            pthread_mutex_unlock(&dns_lock);
            break;
        }
        dns_task_t *task = dns_queue_head;
        dns_queue_head = task->next;
        if (dns_queue_head == NULL) {
            dns_queue_tail = NULL;
        }
        pthread_mutex_unlock(&dns_lock);

        // Perform name resolution
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        bitun_osal_dns_result_t result;
        memset(&result, 0, sizeof(result));
        result.channel_id = task->channel_id;

        int s = getaddrinfo(task->domain, NULL, &hints, &res);
        if (s == 0 && res != NULL) {
            result.success = 1;
            // Allocate space for resolved address
            result.resolved_addr = malloc(res->ai_addrlen);
            if (result.resolved_addr) {
                memcpy(result.resolved_addr, res->ai_addr, res->ai_addrlen);
            }
            // Populate fast IPv4 address if applicable
            if (res->ai_family == AF_INET) {
                struct sockaddr_in *addr_in = (struct sockaddr_in *)res->ai_addr;
                memcpy(result.resolved_ipv4, &addr_in->sin_addr.s_addr, 4);
            }
            freeaddrinfo(res);
        } else {
            result.success = 0;
            result.resolved_addr = NULL;
        }

        // Push results to the caller's queue (writes to the queue's eventfd)
        if (bitun_osal_queue_push(task->result_queue, &result) != 0) {
            if (result.resolved_addr) {
                free(result.resolved_addr);
            }
        }

        free(task);
    }
    return NULL;
}

int bitun_osal_dns_init(void) {
    pthread_mutex_lock(&dns_lock);
    if (dns_thread_running) {
        pthread_mutex_unlock(&dns_lock);
        return 0;
    }
    dns_should_stop = 0;
    dns_queue_head = NULL;
    dns_queue_tail = NULL;
    int ret = pthread_create(&dns_thread, NULL, dns_worker_thread, NULL);
    if (ret == 0) {
        dns_thread_running = 1;
    }
    pthread_mutex_unlock(&dns_lock);
    return (ret == 0) ? 0 : -1;
}

void bitun_osal_dns_deinit(void) {
    pthread_mutex_lock(&dns_lock);
    if (!dns_thread_running) {
        pthread_mutex_unlock(&dns_lock);
        return;
    }
    dns_should_stop = 1;
    pthread_cond_signal(&dns_cond);
    pthread_mutex_unlock(&dns_lock);

    pthread_join(dns_thread, NULL);

    pthread_mutex_lock(&dns_lock);
    dns_thread_running = 0;
    // Clear remaining tasks
    dns_task_t *curr = dns_queue_head;
    while (curr) {
        dns_task_t *next = curr->next;
        free(curr);
        curr = next;
    }
    dns_queue_head = NULL;
    dns_queue_tail = NULL;
    pthread_mutex_unlock(&dns_lock);
}

int bitun_osal_dns_resolve_async(const char *domain, uint32_t channel_id, 
                                 bitun_osal_queue_t *result_queue) {
    if (!domain || !result_queue) return -1;
    dns_task_t *task = malloc(sizeof(dns_task_t));
    if (!task) return -1;
    strncpy(task->domain, domain, sizeof(task->domain) - 1);
    task->domain[sizeof(task->domain) - 1] = '\0';
    task->channel_id = channel_id;
    task->result_queue = result_queue;
    task->next = NULL;

    pthread_mutex_lock(&dns_lock);
    if (!dns_thread_running) {
        pthread_mutex_unlock(&dns_lock);
        free(task);
        return -1;
    }
    if (dns_queue_tail) {
        dns_queue_tail->next = task;
        dns_queue_tail = task;
    } else {
        dns_queue_head = task;
        dns_queue_tail = task;
    }
    pthread_cond_signal(&dns_cond);
    pthread_mutex_unlock(&dns_lock);
    return 0;
}

/* ========================================================================== */
/* 6. 统一密码学加解密与认证接口                                                */
/* ========================================================================== */

int bitun_osal_crypto_hmac_sha256(const uint8_t *key, size_t key_len,
                                  const uint8_t *data, size_t data_len,
                                  uint8_t *mac_out) {
    unsigned int mac_len = 0;
    HMAC(EVP_sha256(), key, key_len, data, data_len, mac_out, &mac_len);
    return (mac_len == 32) ? 0 : -1;
}

int bitun_osal_crypto_hkdf_sha256(const uint8_t *salt, size_t salt_len,
                                  const uint8_t *ikm, size_t ikm_len,
                                  const uint8_t *info, size_t info_len,
                                  uint8_t *okm_out, size_t okm_len) {
    uint8_t zero_salt[32];
    const uint8_t *actual_salt = salt;
    size_t actual_salt_len = salt_len;
    
    if (salt == NULL || salt_len == 0) {
        memset(zero_salt, 0, 32);
        actual_salt = zero_salt;
        actual_salt_len = 32;
    }
    
    uint8_t prk[32];
    unsigned int prk_len = 0;
    HMAC(EVP_sha256(), actual_salt, actual_salt_len, ikm, ikm_len, prk, &prk_len);
    if (prk_len != 32) return -1;

    size_t generated = 0;
    uint8_t t[32];
    size_t t_len = 0;
    uint8_t counter = 1;
    
    if (okm_len > 255 * 32) return -1;

    while (generated < okm_len) {
        HMAC_CTX *ctx = HMAC_CTX_new();
        if (!ctx) return -1;
        
        if (HMAC_Init_ex(ctx, prk, 32, EVP_sha256(), NULL) != 1) {
            HMAC_CTX_free(ctx);
            return -1;
        }
        if (t_len > 0) {
            if (HMAC_Update(ctx, t, t_len) != 1) {
                HMAC_CTX_free(ctx);
                return -1;
            }
        }
        if (info && info_len > 0) {
            if (HMAC_Update(ctx, info, info_len) != 1) {
                HMAC_CTX_free(ctx);
                return -1;
            }
        }
        if (HMAC_Update(ctx, &counter, 1) != 1) {
            HMAC_CTX_free(ctx);
            return -1;
        }
        
        unsigned int mac_len = 0;
        if (HMAC_Final(ctx, t, &mac_len) != 1) {
            HMAC_CTX_free(ctx);
            return -1;
        }
        HMAC_CTX_free(ctx);
        
        t_len = mac_len;
        
        size_t to_copy = okm_len - generated;
        if (to_copy > 32) to_copy = 32;
        memcpy(okm_out + generated, t, to_copy);
        generated += to_copy;
        counter++;
    }
    return 0;
}

int bitun_osal_crypto_chacha20_poly1305_encrypt(const uint8_t *key, const uint8_t *nonce,
                                                const uint8_t *plaintext, size_t plaintext_len,
                                                uint8_t *ciphertext_out, uint8_t *tag_out) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int len = 0;
    int ciphertext_len = 0;

    if (EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL) != 1) goto err;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, BITUN_AEAD_NONCE_LEN, NULL) != 1) goto err;
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) goto err;
    if (EVP_EncryptUpdate(ctx, ciphertext_out, &len, plaintext, plaintext_len) != 1) goto err;
    ciphertext_len = len;

    if (EVP_EncryptFinal_ex(ctx, ciphertext_out + len, &len) != 1) goto err;
    ciphertext_len += len;

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, BITUN_AEAD_TAG_LEN, tag_out) != 1) goto err;

    EVP_CIPHER_CTX_free(ctx);
    return ciphertext_len;

err:
    EVP_CIPHER_CTX_free(ctx);
    return -1;
}

int bitun_osal_crypto_chacha20_poly1305_decrypt(const uint8_t *key, const uint8_t *nonce,
                                                const uint8_t *ciphertext, size_t ciphertext_len,
                                                const uint8_t *tag, uint8_t *plaintext_out) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int len = 0;
    int plaintext_len = 0;
    int ret = 0;

    if (EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL) != 1) goto err;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, BITUN_AEAD_NONCE_LEN, NULL) != 1) goto err;
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) goto err;
    if (EVP_DecryptUpdate(ctx, plaintext_out, &len, ciphertext, ciphertext_len) != 1) goto err;
    plaintext_len = len;

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, BITUN_AEAD_TAG_LEN, (void *)tag) != 1) goto err;

    ret = EVP_DecryptFinal_ex(ctx, plaintext_out + len, &len);
    plaintext_len += len;

    EVP_CIPHER_CTX_free(ctx);
    return (ret > 0) ? plaintext_len : -1;

err:
    EVP_CIPHER_CTX_free(ctx);
    return -1;
}

/* ========================================================================== */
/* 7. 系统时钟与随机数接口                                                     */
/* ========================================================================== */

uint64_t bitun_osal_time_get_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

uint64_t bitun_osal_time_get_real_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void bitun_osal_random_bytes(uint8_t *buf, size_t len) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t r = fread(buf, 1, len, f);
        fclose(f);
        if (r == len) return;
    }
    
    // Fallback
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(rand() & 0xFF);
    }
}

uint32_t bitun_osal_random_u32(void) {
    uint32_t val;
    bitun_osal_random_bytes((uint8_t *)&val, sizeof(val));
    return val;
}
