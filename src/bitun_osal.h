/**
 * @file bitun_osal.h
 * @brief Cross-Platform OS Abstraction Layer for BiTun (Linux & ESP32)
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
/* 1. 系统限制与基本类型定义                                                    */
/* ========================================================================== */

#define BITUN_INVALID_SOCKET (-1)
typedef int bitun_socket_t;

/* 事件多路复用标志定义 (等价映射于 EPOLLIN/EPOLLOUT 或 POLLIN/POLLOUT) */
#define BITUN_POLL_IN   0x0001
#define BITUN_POLL_OUT  0x0004
#define BITUN_POLL_ERR  0x0008

typedef struct {
    bitun_socket_t fd;
    uint32_t events;   /* 输入：关注事件类型，输出：触发的事件类型 */
} bitun_osal_event_t;

/* OSAL 组件的不透明结构体声明 */
typedef struct bitun_osal_poll_set bitun_osal_poll_set_t;
typedef struct bitun_osal_thread   bitun_osal_thread_t;
typedef struct bitun_osal_mutex    bitun_osal_mutex_t;
typedef struct bitun_osal_queue    bitun_osal_queue_t;

/* 套接字地址结构前置声明 */
struct sockaddr;
typedef unsigned int bitun_socklen_t;

/* 异步 DNS 解析结果结构体 */
typedef struct {
    uint32_t channel_id;
    struct sockaddr *resolved_addr; /* 动态分配的解析后地址，消费完后需手动 free */
    uint8_t resolved_ipv4[4];       /* 快速获取的 IPv4 地址 */
    int success;
} bitun_osal_dns_result_t;

/* ========================================================================== */
/* 2. 跨平台字节序转换工具                                                     */
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
    /* ESP32 / Generic Newlib 平台的可移植手动实现 */
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
/* 3. 套接字网络抽象 API                                                      */
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
/* 4. 多路复用事件监听接口 (Epoll/Poll 统一封装)                                  */
/* ========================================================================== */

bitun_osal_poll_set_t *bitun_osal_poll_create(void);
void bitun_osal_poll_destroy(bitun_osal_poll_set_t *set);
int bitun_osal_poll_add(bitun_osal_poll_set_t *set, bitun_socket_t fd, uint32_t events);
int bitun_osal_poll_mod(bitun_osal_poll_set_t *set, bitun_socket_t fd, uint32_t events);
int bitun_osal_poll_del(bitun_osal_poll_set_t *set, bitun_socket_t fd);
int bitun_osal_poll_wait(bitun_osal_poll_set_t *set, int timeout_ms, 
                         bitun_osal_event_t *events_out, int max_events);

/* ========================================================================== */
/* 5. 线程与同步互斥锁接口                                                     */
/* ========================================================================== */

typedef void *(*bitun_osal_thread_entry_t)(void *arg);

/**
 * @brief 跨平台多线程创建
 * @param stack_size 线程任务栈大小 (在 ESP32 上用于避免溢出，在 Linux 上被忽略)
 * @param priority 线程调度优先级 (在 FreeRTOS 上生效，在 Linux 上映射为默认调度属性)
 */
int bitun_osal_thread_create(bitun_osal_thread_t **thread_out, const char *name, 
                             uint32_t stack_size, uint32_t priority,
                             bitun_osal_thread_entry_t entry, void *arg);
int bitun_osal_thread_detach(bitun_osal_thread_t *thread);
void bitun_osal_thread_sleep_ms(uint32_t ms);

/* 互斥锁 API */
int bitun_osal_mutex_create(bitun_osal_mutex_t **mutex_out);
int bitun_osal_mutex_lock(bitun_osal_mutex_t *mutex);
int bitun_osal_mutex_unlock(bitun_osal_mutex_t *mutex);
int bitun_osal_mutex_destroy(bitun_osal_mutex_t *mutex);

/* ========================================================================== */
/* 6. eventfd 跨线程/进程通信队列 (主循环物理自唤醒)                               */
/* ========================================================================== */

/**
 * @brief 线程安全的文件描述符邮箱队列。推入数据时向 eventfd 写入，从而唤醒处于 poll_wait 的主循环
 */
bitun_osal_queue_t *bitun_osal_queue_create(size_t item_size, size_t capacity);
void bitun_osal_queue_destroy(bitun_osal_queue_t *q);
int bitun_osal_queue_push(bitun_osal_queue_t *q, const void *item);
int bitun_osal_queue_pop(bitun_osal_queue_t *q, void *item_out);
/* 获取队列底层的 eventfd，用于将其注册进主循环的多路复用监听集 */
bitun_socket_t bitun_osal_queue_get_read_fd(bitun_osal_queue_t *q);
/* 清除 eventfd 上的唤醒字节计数 (读取 8 字节计数器值) */
void bitun_osal_queue_clear_wakeup(bitun_osal_queue_t *q);

/* ========================================================================== */
/* 7. 全局异步 DNS 解析系统 (固定堆栈上限设计)                                   */
/* ========================================================================== */

/**
 * @brief 初始化全局唯一的 DNS 工作任务和内部缓冲请求队列
 */
int bitun_osal_dns_init(void);

/**
 * @brief 释放 DNS 解析器资源并销毁工作线程
 */
void bitun_osal_dns_deinit(void);

/**
 * @brief 将解析请求排队投递给 DNS 工作任务
 * @param domain 解析的目标域名
 * @param channel_id 绑定的通信通道 ID
 * @param result_queue 存放解析结果的 eventfd 通信队列
 */
int bitun_osal_dns_resolve_async(const char *domain, uint32_t channel_id, 
                                 bitun_osal_queue_t *result_queue);

/* ========================================================================== */
/* 8. 统一密码学加解密与认证接口                                                */
/* ========================================================================== */

#define BITUN_AEAD_TAG_LEN   16
#define BITUN_AEAD_NONCE_LEN 12

/**
 * @brief HMAC-SHA256 签名计算
 */
int bitun_osal_crypto_hmac_sha256(const uint8_t *key, size_t key_len,
                                  const uint8_t *data, size_t data_len,
                                  uint8_t *mac_out);

/**
 * @brief HKDF-SHA256 密钥派生
 */
int bitun_osal_crypto_hkdf_sha256(const uint8_t *salt, size_t salt_len,
                                  const uint8_t *ikm, size_t ikm_len,
                                  const uint8_t *info, size_t info_len,
                                  uint8_t *okm_out, size_t okm_len);

/**
 * @brief ChaCha20-Poly1305 认证加密
 */
int bitun_osal_crypto_chacha20_poly1305_encrypt(const uint8_t *key, const uint8_t *nonce,
                                                const uint8_t *plaintext, size_t plaintext_len,
                                                uint8_t *ciphertext_out, uint8_t *tag_out);

/**
 * @brief ChaCha20-Poly1305 认证解密校验
 * @note 必须完美支持 In-place（即 ciphertext == plaintext_out）。禁止传入部分重叠缓冲区。
 */
int bitun_osal_crypto_chacha20_poly1305_decrypt(const uint8_t *key, const uint8_t *nonce,
                                                const uint8_t *ciphertext, size_t ciphertext_len,
                                                const uint8_t *tag, uint8_t *plaintext_out);

/* ========================================================================== */
/* 9. 系统单调时钟与随机数生成接口                                              */
/* ========================================================================== */

/**
 * @brief 获取微秒/毫秒级开机单调时间 (KCP 时钟滴答)
 */
uint64_t bitun_osal_time_get_ms(void);

/**
 * @brief 获取 Unix Epoch 真实日历时间
 */
uint64_t bitun_osal_time_get_real_ms(void);

/**
 * @brief 获取硬件/系统随机 32 位无符号整数
 */
uint32_t bitun_osal_random_u32(void);

/**
 * @brief 生成强安全随机数填充缓冲区
 */
void bitun_osal_random_bytes(uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* BITUN_OSAL_H */
