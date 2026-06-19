# BiTun 移植依赖性分析报告 (Linux vs ESP32) - 修正版

本报告针对 BiTun 项目在 Linux 平台下的源码进行系统依赖性分析，并结合质控反馈对多路复用 LT 忙轮询、SRAM 资源开销、解密内存重叠和异步 DNS 解析等四个核心缺陷进行了深度分析与方案修正。

---

## 1. 核心缺陷答辩与修正方案

### 缺陷 1：Epoll ET 与 Poll LT 语义冲突及系统死锁/忙轮询风险
*   **问题分析**：
    原版 `tunnel.c` 在启动非阻塞连接 `connect()` 后，将套接字以 `EPOLLIN | EPOLLOUT | EPOLLET` 注册到 epoll 中 [[tunnel.c:L479](file:///home/chenming/BiTun/src/tunnel.c#L479), [tunnel.c:L960](file:///home/chenming/BiTun/src/tunnel.c#L960)]。在 Linux 边缘触发（ET）模式下，写缓冲区就绪事件（`EPOLLOUT`）只会触发一次。
    然而，在 ESP32 (LwIP) 下只有水平触发（LT）的 `poll()`。当连接建立成功后，由于 TCP 发送缓冲区几乎一直处于空闲可写状态，`poll()` 会持续返回可写事件（`POLLOUT`）。原版事件循环又没有处理 `EPOLLOUT` 的逻辑 [[tunnel.c:L782-L875](file:///home/chenming/BiTun/src/tunnel.c#L782-L875)]，这将导致 `poll()` 瞬间陷入 **100% CPU 占用** 的忙轮询暴风雨。
*   **修正设计**：
    我们需要处理并消费 `POLLOUT` 事件，在非阻塞 `connect` 建立成功后立即移除对该事件的监听。修改逻辑如下：
    1.  当 `poll_wait` 检测到某个 TCP 通道套接字触发了 `POLLOUT` 时，调用 `getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len)`。
    2.  若 `err == 0`，表明连接成功，主循环立即发送 `CMD_CONNECT_ACK` (0x00) 给对端，并立即调用 `bitun_osal_poll_mod(..., fd, BITUN_POLL_IN)` **注销可写监听**，仅保留可读监听。
    3.  若 `err != 0`，表明连接失败，发送 `CMD_CONNECT_ACK` (0x01) 并关闭通道。
    4.  这不仅规避了 LT 模式下的可写事件忙轮询，还顺带**修复了原版代码在连接成功时未向对端发送 `CMD_CONNECT_ACK(0x00)` 的严重逻辑缺陷**。

### 缺陷 2：SRAM 资源约束与本地套接字对自唤醒高额开销
*   **问题分析**：
    原设计使用一对 UDP loopback 套接字来实现 DNS 异步解析完成时的物理唤醒。但这会消耗 2 个 FD，在 LwIP 中还会创建 2 个 `udp_pcb` 结构体和关联的 `recvmbox` 邮箱队列，这不仅带来了 1.5KB 以上的额外 RAM 消耗，还经历了 LwIP 完整的协议栈回路路由。
*   **eventfd 替代方案**：
    ESP-IDF v5.x 原生支持标准的 POSIX `eventfd`（包含在 `<sys/eventfd.h>` 中，通过 `esp_vfs_eventfd_register()` 注册）。
    1.  **资源开销**：`eventfd` 仅占用 **1 个描述符**（FD），仅消耗几百字节的轻量结构体，无任何网络协议栈开销。
    2.  **设计实现**：OSAL 队列 `bitun_osal_queue_t` 内部持有一个 `eventfd`。当 DNS 解析线程向队列 `push` 解析结果时，调用 `write(evfd, &val, 8)`（写入 `1`）。主循环在 `poll_wait` 中监听该 `evfd` 的 `POLLIN` 事件。一旦被唤醒，主循环读取并处理解析结果，随后调用 `read(evfd, &val, 8)` 清除事件，机制极度轻量。

### 缺陷 3：密码学接口的内存拷贝与部分指针重叠安全隐患
*   **问题分析**：
    原版 `tunnel.c` 在解密时，密文输入指针为 `read_buf + 36`，解密输出目标指针为 `read_buf` [[tunnel.c:L691](file:///home/chenming/BiTun/src/tunnel.c#L691)]。这两个缓冲区有 36 字节的部分重叠。OpenSSL 会触发 `EVP_R_PARTIALLY_OVERLAPPING` 错误直接拒绝解密，而在 mbedTLS 中这属于未定义行为（可能会损坏尚未解密的密文段）。
*   **原位 (In-place) 解密 + 安全搬移方案**：
    在 OSAL 的 ChaCha20-Poly1305 接口中，显式规定必须支持 **原位加解密**（即输入指针等于输出指针：`ciphertext == plaintext_out`），OpenSSL 与 mbedTLS（包括 ESP32 的硬件加速驱动）对此均有完美的原生支持。
    1.  `tunnel.c` 在调用 `decrypt_chacha20_poly1305` 时，将输入和输出指针均设为 `read_buf + 36`：
        ```c
        int dec_len = decrypt_chacha20_poly1305(key, nonce, read_buf + 36, len, tag, read_buf + 36);
        ```
    2.  解密完成后，`tunnel.c` 内部调用 C 标准库安全的 `memmove` 将明文安全地向前平移 36 字节：
        ```c
        if (dec_len > 0) {
            memmove(read_buf, read_buf + 36, dec_len);
        }
        ```
    3.  此方案**零堆内存分配**，彻底规避了重叠安全隐患，且在 Linux 和 ESP32 平台均能获得最高性能。

### 缺陷 4：线程级异步 DNS 任务对 RAM 的严重浪费
*   **问题分析**：
    原版为每个 DNS 请求单独创建一个线程 [[tunnel.c:L990](file:///home/chenming/BiTun/src/tunnel.c#L990)]。在 ESP32 上，若同时发起多个通道的域名解析，频繁地动态创建和销毁任务（每个任务至少需 3KB 堆栈）极易导致内存碎片化并引发 OOM。
*   **全局单一 DNS 工作任务方案**：
    为了保证极简的跨平台可移植性，不采用底层深度绑定的 LwIP 回调接口，而是采用 OSAL 内置的**全局单工作线程解析排队机制**：
    1.  OSAL 在初始化时创建一个常驻的低优先级 DNS 解析工作线程（如任务栈大小固定为 4KB），并创建一个 thread-safe 的内部 DNS 请求队列。
    2.  当通道请求域名解析时，不再创建线程，而是将包含域名、通道 ID、返回队列的请求结构体推入内部的 DNS 请求队列。
    3.  全局 DNS 解析线程从请求队列中顺序读取域名，调用平台标准的 `getaddrinfo`（在 Linux 和 ESP32-IDF 下皆为线程安全实现）进行阻塞式解析，然后将解析结果推入返回队列以唤醒主事件循环。
    4.  **优势**：无论并发多少个通道，内存开销**永远固定在 1 个任务的堆栈开销（约 4KB）**，实现了内存占用的硬上限保护，且与平台无关。

---

## 2. 源码系统依赖性扫描更新

基于对以上方案的修正，我们重新梳理了平台底层的 API 依赖关系：

| 类别 | Linux 平台实现 | ESP32 平台实现 | OSAL 封装接口 |
| :--- | :--- | :--- | :--- |
| **套接字网络库** | BSD Sockets | LwIP Sockets | `bitun_osal_socket_*` |
| **多路复用** | `epoll` + `POLLOUT` 连接状态捕获 | `poll()` + `POLLOUT` 连接状态捕获 | `bitun_osal_poll_set_t` |
| **进程内唤醒** | `eventfd()` 机制 | `eventfd()` 机制 (ESP-IDF 虚拟文件系统注册) | `bitun_osal_queue_t` |
| **并发多线程** | POSIX `pthread` | FreeRTOS Tasks | `bitun_osal_thread_*` |
| **加密与签名** | OpenSSL EVP / HMAC | mbedTLS MD / ChaChaPoly | `bitun_osal_crypto_*` |
| **异步解析** | 全局单一 DNS 工作线程 | 全局单一 DNS 工作任务 | `bitun_osal_dns_resolve_async` |
| **系统时钟** | `clock_gettime(CLOCK_MONOTONIC)` | `esp_timer_get_time() / 1000` | `bitun_osal_time_get_ms` |
| **系统随机数** | `random()` | `esp_random()` (硬件真随机数) | `bitun_osal_random_bytes` |
