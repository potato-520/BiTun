# BiTun ESP32 移植与代码复用设计指南

本指南旨在指导如何将 Linux 端的 BiTun 纯 C 源码以最大复用率移植到 ESP32 (ESP-IDF) 嵌入式开发环境。通过建立平台抽象层（PAL），我们可以确保核心协议（KCP、SOCKS5 解析、防重放滑动窗口）完全无需修改，仅需替换底层的网络事件多路复用与密码学计算库。

---

## 1. 软件架构设计 (Software PAL Architecture)

为了实现两端代码的解耦与最大复用，建议的系统软件栈如下：

```
+-------------------------------------------------------------+
|               应用层业务逻辑 (main / 状态机 / 心跳)           |
+-------------------------------------------------------------+
|       Channel Multiplexing (分路多路复用层 - 纯C无依赖)      |
|           (ikcp.c, socks5.c, 奇偶 ID 通道分配逻辑)           |
+-------------------------------------------------------------+
|    密码学抽象接口 (encrypt.h)  |    网络与事件抽象接口 (tunnel.h) |
+-------------------------------+-----------------------------+
|         [平台抽象层 (PAL) 实现 - 条件编译/分流分包]          |
|   - Linux: OpenSSL            |   - Linux: Epoll ET / POSIX  |
|   - ESP32: mbedTLS            |   - ESP32: LwIP Select / RTOS|
+-------------------------------------------------------------+
|                      底 层 操 作 系 统                       |
|   - Linux (POSIX / Threads)   |   - ESP32 (FreeRTOS / LwIP) |
+-------------------------------------------------------------+
```

---

## 2. 核心模块复用与重构策略

### 2.1 核心协议模块 (100% 直接复用)
以下文件属于纯 C 实现，不依赖任何特定操作系统的套接字或线程库，可直接拷贝并在 ESP32 编译：
*   **`src/ikcp.c` 和 `src/ikcp.h`**：KCP 协议核心。只需在构建系统中进行调优配置。
*   **`src/socks5.c` 和 `src/socks5.h`**：流式无状态 SOCKS5 状态机。它仅对内存缓冲区操作，完美适配 LwIP 数据流。
*   **防重放滑动窗口**：`src/encrypt.c` 中的 `anti_replay_check` 和 `anti_replay_update` 均为 64 位整型位运算，无任何系统依赖，可直接复用。

### 2.2 密码学计算模块 (Crypto PAL 重构)
*   **差异**：Linux 依赖 OpenSSL `libcrypto`，ESP32 依赖 `mbedTLS`。
*   **改造方案**：
    1.  规范 `src/encrypt.h`，对外仅暴露算法接口，屏蔽底层库特定的 Context 结构。
    2.  根据编译宏 `ESP_PLATFORM` 编写两套密码学包装实现：
        *   `src/encrypt_openssl.c` (用于 Linux 编译)
        *   `src/encrypt_mbedtls.c` (用于 ESP32 编译)
    3.  在 ESP32 上调用 mbedTLS 对应的 ChaCha20-Poly1305、HKDF-SHA256 和 HMAC-SHA256 接口，ESP-IDF 的 mbedTLS 硬件驱动会自动启用物理加密芯片加速。

### 2.3 网络与多路复用模块 (Event Loop 重构)
*   **差异**：Linux 强依赖 `epoll` ET 模式和 Unix 管道；ESP32 仅有 LwIP 实现的 BSD socket，支持 `select()` 且无 `epoll`。
*   **改造方案**：
    由于 Linux 和 ESP32 运行环境的物理限制差异巨大，建议将网络事件循环划分为两套实现文件：
    1.  **`src/tunnel_linux.c`**：保留基于 Linux Epoll + Edge-Triggered + Unix Self-Pipe + Pthread 的高性能、高并发实现。
    2.  **`src/tunnel_esp32.c`**：针对 ESP-IDF 平台特性重写：
        *   **事件多路复用**：使用标准 LwIP `select()` 监听已绑定的 TCP/UDP 套接字。因为 ESP32 客户端的并发连接数极低（通常通道数 $<10$），`select` 的性能和资源消耗最为契合。
        *   **任务管理**：使用 FreeRTOS 任务 `vTaskCreate` 代替 `pthread`。
        *   **异步 DNS**：丢弃 Linux 的自管道设计。直接调用 LwIP 的非阻塞 API `dns_gethostbyname()`。在回调函数中向主任务的 FreeRTOS 事件组（Event Group）或队列（Queue）写入 `dns_result_t`，避免死锁和 UAF。
        *   **背压机制**：KCP 发送缓冲区的背压依然通过检查 `ikcp_waitsnd` 并在超过安全阈值（如 32 包）时，挂起或继续调用 FreeRTOS socket 的读事件来实现。

---

## 3. 内存与背压配置调优 (ESP32 RAM Tuning)

ESP32 堆内存（SRAM）极其有限（约 200KB - 300KB 可用），容易因积压爆发引发 OOM。
因此在 ESP32 下，需要对 KCP 窗口和缓冲区进行微调：

1.  **限制 KCP 窗口大小**：
    *   将 KCP 的发送与接收窗口 `ikcp_wndsize` 设为 `32` 或 `16`（Linux 侧可以为 `128`）。
2.  **降低单通道读取配额**：
    *   将每次读取 TCP socket 送入 KCP 的配额降为 `1KB` 或 `2KB`，减少瞬时 Heap 占用。
3.  **内存分配选型**：
    *   如果 ESP32 开发板配备了外部 PSRAM (SPI RAM)，对于 KCP 的大块缓冲区，可以使用 ESP-IDF 的 `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` 进行堆内存分配。如果没有 PSRAM，则必须严格控制 KCP 的待发队列和 Channel 数量限制（例如最多限制 8 个并发 TCP Channel）。

---

## 4. CMake 编译条件分流示例

在组件的 `CMakeLists.txt` 中，我们可以直接通过 CMake 来控制不同平台的编译源文件：

```cmake
# ESP-IDF CMakeLists.txt (BareTcl_BiTun_shared 下的 CMake 构建脚本)
if(NOT ESP_PLATFORM)
    # 本地 Linux 测试工程编译
    set(SOURCES 
        src/ikcp.c
        src/socks5.c
        src/encrypt_openssl.c
        src/tunnel_linux.c
        src/main.c
    )
else()
    # ESP32 联合固件编译
    set(SOURCES 
        src/ikcp.c
        src/socks5.c
        src/encrypt_mbedtls.c
        src/tunnel_esp32.c
    )
    idf_component_register(SRCS ${SOURCES}
                        INCLUDE_DIRS "src"
                        REQUIRES mbedtls lwip)
endif()
```

这样我们成功做到了在享受 Linux 高并发事件驱动模型的同时，完全不改动协议核心，就能编译出极度精简、安全且稳健的 ESP32 嵌入式双向隧道固件。
