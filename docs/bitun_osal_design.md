# BiTun 统一 OSAL 架构设计及接口定义 - 修正版

本报告定义了修正后的 BiTun 跨平台操作系统抽象层（OSAL）接口。针对 LwIP Level Triggered 多路复用忙轮询风险、ESP32 SRAM 资源约束、加密内存部分重叠和高并发 DNS 解析线程开销等四个核心隐患给出了优化设计。

---

## 1. OSAL 关键子系统详细设计

### 1.1 eventfd 唤醒队列设计 (`bitun_osal_queue_t`)
为了替代高开销的 UDP loopback 套接字对，设计了基于标准 `eventfd` 的轻量级队列：

*   **队列结构**：
    内部持有一个 mutex 锁保护的环形缓冲区，以及一个 `eventfd` 句柄。
*   **注册 poll_set**：
    通过 `bitun_osal_queue_get_read_fd(q)` 导出 `eventfd` 的句柄，主事件循环通过 `bitun_osal_poll_add` 将该句柄加入 `poll_set`，监听 `BITUN_POLL_IN` 事件。
*   **发送端 (Push)**：
    当异步任务完成数据生产（如 DNS 结果解析完成），首先锁定 mutex 将数据推入环形队列，然后向 `eventfd` 写入一个 8 字节整数 `val = 1`：
    ```c
    uint64_t val = 1;
    write(evfd, &val, sizeof(val));
    ```
    此时，操作系统的内核将物理唤醒被阻塞在 `poll_wait` 中的主事件循环。
*   **接收端 (Pop & Clear)**：
    主事件循环被唤醒后，首先调用 `bitun_osal_queue_clear_wakeup(q)` 消费 `eventfd` 内部的值（读取 8 字节并复位）：
    ```c
    uint64_t val = 0;
    read(evfd, &val, sizeof(val));
    ```
    然后锁定 mutex，安全地从环形缓冲区中取出数据项进行业务处理。
*   **ESP32 适配**：
    在 ESP-IDF 平台下，在初始化队列时，自动调用 `esp_vfs_eventfd_register()` 注册 eventfd 虚拟文件系统接口。此机制开销极低且极其稳定。

### 1.2 全局单一 DNS 工作线程设计
为了规避并发通道解析域名时带来的高额任务创建与堆栈开销，设计了全局单一常驻 DNS 任务：

1.  在系统启动时，调用 `bitun_osal_dns_init()` 初始化全局 DNS 解析子系统。
2.  内部创建一个常驻的低优先级 FreeRTOS 任务（Linux 上为 pthread 线程），以及一个 thread-safe 的内部 DNS 请求队列。
3.  业务层需要域名解析时，调用 `bitun_osal_dns_resolve_async(domain, channel_id, result_queue)`。该接口仅将请求封装并推入内部的请求队列（零阻塞，不创建任何新线程）。
4.  全局 DNS 工作任务从队列中取出请求，调用系统标准的 `getaddrinfo` 完成域名解析后，将包含结果的结构体推入通道对应的 `result_queue`，并通过 `eventfd` 物理唤醒主事件循环。
5.  系统销毁时，调用 `bitun_osal_dns_deinit()` 优雅释放队列和销毁工作线程。

### 1.3 密码学原位 (In-place) 解密安全设计
*   在 `bitun_osal_crypto_chacha20_poly1305_decrypt` 中，要求底层加解密引擎必须支持 `plaintext_out == ciphertext`。
*   **严禁部分重叠指针**：传入的密文指针和明文输出指针必须 **完全一致**（原位解密）或 **完全不重叠**，严禁使用部分重叠的指针偏移解密。
*   主业务层 `tunnel.c` 必须通过 in-place 解密后配合 `memmove` 进行数据移动，从而规避指针部分重叠引起的安全报错。

---

## 2. 更新后的 `bitun_osal.h` 完整接口定义

```c
/**
 * @file bitun_osal.h
 * @brief Cross-Platform OS Abstraction Layer for BiTun (Linux & ESP32) - Updated Version
 */

#ifndef BITUN_OSAL_H
#define BITUN_OSAL_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* 1. System Limits and Types                                                */
/* ========================================================================== */

#define BITUN_INVALID_SOCKET (-1)
typedef int bitun_socket_t;

/* Define Poll Event types (corresponds to EPOLLIN/EPOLLOUT and POLLIN/POLLOUT) */
#define BITUN_POLL_IN   0x0001
#define BITUN_POLL_OUT  0x0004
#define BITUN_POLL_ERR  0x0008

typedef struct {
    bitun_socket_t fd;
    uint32_t events;   /* Input: events to watch, Output: occurred events */
} bitun_osal_event_t;

/* Opaque structures for OSAL components */
typedef struct bitun_osal_poll_set bitun_osal_poll_set_t;
typedef struct bitun_osal_thread   bitun_osal_thread_t;
typedef struct bitun_osal_mutex    bitun_osal_mutex_t;
typedef struct bitun_osal_queue    bitun_osal_queue_t;

/* Forward declaration of sockaddr for networking APIs */
struct sockaddr;
typedef unsigned int bitun_socklen_t;

/* Structure for DNS async resolution results */
typedef struct {
    uint32_t channel_id;
    struct sockaddr *resolved_addr; /* Dynamically allocated, must be freed by consumer or local buffer */
    uint8_t resolved_ipv4[4];       /* Simplified IPv4 address for quick access */
    int success;
} bitun_osal_dns_result_t;

/* ========================================================================== */
/* 2. Endianness Utilities                                                   */
/* ========================================================================== */

#if defined(__linux__)
    #include <endian.h>
    #define bitun_htobe16(x) htobe16(x)
    #define bitun_be16toh(x) be16toh(x)
    #define bitun_htobe32(x) htobe32(x)
    #define bitun_be32toh(x) be32toh(x)
    #define bitun_htobe64(x) htobe64(x)
    #define bitun_be64toh(x) be64toh(x)
#else
    /* Portable manual fallback implementations for ESP32 / generic Newlib */
    #include <sys/param.h>
    #if __BYTE_ORDER == __LITTLE_ENDIAN
        static inline uint16_t bitun_htobe16(uint16_t x) { return __builtin_bswap16(x); }
        static inline uint16_t bitun_be16toh(uint16_t x) { return __builtin_bswap16(x); }
        static inline uint32_t bitun_htobe32(uint32_t x) { return __builtin_bswap32(x); }
        static inline uint32_t bitun_be32toh(uint32_t x) { return __builtin_bswap32(x); }
        static inline uint64_t bitun_htobe64(uint64_t x) { return __builtin_bswap64(x); }
        static inline uint64_t bitun_be64toh(uint64_t x) { return __builtin_bswap64(x); }
    #else
        #define bitun_htobe16(x) (x)
        #define bitun_be16toh(x) (x)
        #define bitun_htobe32(x) (x)
        #define bitun_be32toh(x) (x)
        #define bitun_htobe64(x) (x)
        #define bitun_be64toh(x) (x)
    #endif
#endif

/* ========================================================================== */
/* 3. Socket & Network APIs                                                  */
/* ========================================================================== */

bitun_socket_t bitun_osal_socket_create(int domain, int type, int protocol);
int bitun_osal_socket_close(bitun_socket_t fd);
int bitun_osal_socket_bind(bitun_socket_t fd, const struct sockaddr *addr, bitun_socklen_t addrlen);
int bitun_osal_socket_listen(bitun_socket_t fd, int backlog);
bitun_socket_t bitun_osal_socket_accept(bitun_socket_t fd, struct sockaddr *addr, bitun_socklen_t *addrlen);
int bitun_osal_socket_connect(bitun_socket_t fd, const struct sockaddr *addr, bitun_socklen_t addrlen);
int bitun_osal_socket_send(bitun_socket_t fd, const void *buf, size_t len, int flags);
int bitun_osal_socket_recv(bitun_socket_t fd, void *buf, size_t len, int flags);
int bitun_osal_socket_sendto(bitun_socket_t fd, const void *buf, size_t len, int flags,
                             const struct sockaddr *dest_addr, bitun_socklen_t addrlen);
int bitun_osal_socket_recvfrom(bitun_socket_t fd, void *buf, size_t len, int flags,
                               struct sockaddr *src_addr, bitun_socklen_t *addrlen);
int bitun_osal_socket_set_nonblocking(bitun_socket_t fd);
int bitun_osal_socket_set_reuseaddr(bitun_socket_t fd);

/* ========================================================================== */
/* 4. Event Multiplexing (Poll/Epoll Abstraction)                            */
/* ========================================================================== */

bitun_osal_poll_set_t *bitun_osal_poll_create(void);
void bitun_osal_poll_destroy(bitun_osal_poll_set_t *set);
int bitun_osal_poll_add(bitun_osal_poll_set_t *set, bitun_socket_t fd, uint32_t events);
int bitun_osal_poll_mod(bitun_osal_poll_set_t *set, bitun_socket_t fd, uint32_t events);
int bitun_osal_poll_del(bitun_osal_poll_set_t *set, bitun_socket_t fd);
int bitun_osal_poll_wait(bitun_osal_poll_set_t *set, int timeout_ms, 
                         bitun_osal_event_t *events_out, int max_events);

/* ========================================================================== */
/* 5. Threading & Synchronization APIs                                       */
/* ========================================================================== */

typedef void *(*bitun_osal_thread_entry_t)(void *arg);

/**
 * @brief Cross-platform thread creation
 * @param stack_size Stack size in bytes (critical for ESP32, ignored on Linux)
 * @param priority Task priority (for FreeRTOS scheduler, mapped to thread policies on Linux)
 */
int bitun_osal_thread_create(bitun_osal_thread_t **thread_out, const char *name, 
                             uint32_t stack_size, uint32_t priority,
                             bitun_osal_thread_entry_t entry, void *arg);
int bitun_osal_thread_detach(bitun_osal_thread_t *thread);
void bitun_osal_thread_sleep_ms(uint32_t ms);

/* Mutex APIs */
int bitun_osal_mutex_create(bitun_osal_mutex_t **mutex_out);
int bitun_osal_mutex_lock(bitun_osal_mutex_t *mutex);
int bitun_osal_mutex_unlock(bitun_osal_mutex_t *mutex);
int bitun_osal_mutex_destroy(bitun_osal_mutex_t *mutex);

/* ========================================================================== */
/* 6. Inter-Thread Communication Queue (eventfd-based)                       */
/* ========================================================================== */

/**
 * @brief Thread-safe mailbox queue that integrates with the poll set.
 * Internally utilizes standard eventfd for wakeup signaling.
 */
bitun_osal_queue_t *bitun_osal_queue_create(size_t item_size, size_t capacity);
void bitun_osal_queue_destroy(bitun_osal_queue_t *q);
int bitun_osal_queue_push(bitun_osal_queue_t *q, const void *item);
int bitun_osal_queue_pop(bitun_osal_queue_t *q, void *item_out);
/* Returns the internal eventfd used for wakeup to add to poll set */
bitun_socket_t bitun_osal_queue_get_read_fd(bitun_osal_queue_t *q);
/* Clears the wakeup signal from eventfd (reads 8-byte value) */
void bitun_osal_queue_clear_wakeup(bitun_osal_queue_t *q);

/* ========================================================================== */
/* 7. Global Async DNS System (Fixed Memory Footprint)                       */
/* ========================================================================== */

/**
 * @brief Initialize the global single-thread DNS worker pool
 */
int bitun_osal_dns_init(void);

/**
 * @brief Deinitialize the global DNS system and release resources
 */
void bitun_osal_dns_deinit(void);

/**
 * @brief Queue an asynchronous DNS resolution request
 * @param domain Target host domain to resolve
 * @param channel_id Channel identifier to associate with the request
 * @param result_queue Main loop eventfd queue where the result structure will be pushed
 */
int bitun_osal_dns_resolve_async(const char *domain, uint32_t channel_id, 
                                 bitun_osal_queue_t *result_queue);

/* ========================================================================== */
/* 8. Cryptographic & Security APIs                                          */
/* ========================================================================== */

#define BITUN_AEAD_TAG_LEN   16
#define BITUN_AEAD_NONCE_LEN 12

/**
 * @brief HMAC-SHA256 calculation
 */
int bitun_osal_crypto_hmac_sha256(const uint8_t *key, size_t key_len,
                                  const uint8_t *data, size_t data_len,
                                  uint8_t *mac_out);

/**
 * @brief HKDF-SHA256 Extract-and-Expand implementation
 */
int bitun_osal_crypto_hkdf_sha256(const uint8_t *salt, size_t salt_len,
                                  const uint8_t *ikm, size_t ikm_len,
                                  const uint8_t *info, size_t info_len,
                                  uint8_t *okm_out, size_t okm_len);

/**
 * @brief ChaCha20-Poly1305 AEAD Authenticated Encryption
 */
int bitun_osal_crypto_chacha20_poly1305_encrypt(const uint8_t *key, const uint8_t *nonce,
                                                const uint8_t *plaintext, size_t plaintext_len,
                                                uint8_t *ciphertext_out, uint8_t *tag_out);

/**
 * @brief ChaCha20-Poly1305 AEAD Authenticated Decryption
 * @note Out-of-place or exact In-place (ciphertext == plaintext_out) are supported.
 *       Partially overlapping buffers are strictly prohibited.
 */
int bitun_osal_crypto_chacha20_poly1305_decrypt(const uint8_t *key, const uint8_t *nonce,
                                                const uint8_t *ciphertext, size_t ciphertext_len,
                                                const uint8_t *tag, uint8_t *plaintext_out);

/* ========================================================================== */
/* 9. System Timing & Random Number Generator                                */
/* ========================================================================== */

/**
 * @brief Get monotonic millisecond timestamp (tick counts)
 */
uint64_t bitun_osal_time_get_ms(void);

/**
 * @brief Get wall-clock calendar millisecond timestamp (Unix epoch time)
 */
uint64_t bitun_osal_time_get_real_ms(void);

/**
 * @brief Get hardware/system generated random 32-bit integer
 */
uint32_t bitun_osal_random_u32(void);

/**
 * @brief Fill buffer with cryptographically secure random bytes
 */
void bitun_osal_random_bytes(uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* BITUN_OSAL_H */
```
