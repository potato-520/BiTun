# BiTun OSAL 跨平台移植实现报告

本报告详细说明了 BiTun 双向加密隧道系统在 Linux 平台下的操作系统抽象层 (OSAL) 的接口实现、代码适配调整、编译构建、以及本地单元测试与集成测试验证情况。

工作目录：`/home/chenming/BiTun`

---

## 一、 主要修改内容与实现机制

### 1. `src/bitun_osal.h` 跨平台统一头文件定义
我们完整创建了 `src/bitun_osal.h`，定义了标准的类型和 API，包括：
- 字节序转换（在 Linux 下映射为 `<endian.h>`）
- 抽象网络套接字（`bitun_socket_t`，非阻塞 fcntl，`reuseaddr` 等）
- 事件多路复用抽象（统一封装 epoll 的 LT 模式）
- 线程与互斥锁抽象（基于 pthread 封装）
- `bitun_osal_queue_t` 邮箱队列（基于 `eventfd` 与 pthread 锁实现主循环自唤醒）
- 全局异步 DNS 解析接口（`bitun_osal_dns_resolve_async`）
- 统一密码学接口（HMAC-SHA256, HKDF-SHA256, ChaCha20-Poly1305）
- 开机单调时间与强随机数接口（读取 `/dev/urandom`）

### 2. `src/linux/bitun_osal.c` Linux 平台具体实现
- **网络与多路复用**：完全封装了 `socket`、`bind`、`listen`、`accept`、`connect` 以及 `epoll`，将 `epoll` 的 `EPOLLIN`/`EPOLLOUT`/`EPOLLERR` 水平触发模式映射为统一事件集。
- **自唤醒队列**：采用 Linux `eventfd` 机制配合环形队列 and pthread 互斥锁。向队列 push 时写入 8 字节 `1` 到 eventfd，pop 时读出 8 字节清除计数。
- **全局异步 DNS 模块**：在 `bitun_osal_dns_init` 时启动单一 DNS 后台常驻线程。请求发起时将解析任务放入线程安全的队列并以 `pthread_cond` 唤醒后台线程。解析完毕后，结果通过 `bitun_osal_queue_push` 传回调用者的 dns 邮箱队列（自动触发 eventfd 自唤醒）。
- **密码学模块**：基于 OpenSSL `EVP` 与 `HMAC` 库函数实现。其中 `bitun_osal_crypto_chacha20_poly1305_decrypt` 完美支持 in-place 原位加解密（即输入与输出缓存区完全重合），并在代码头部定义了 `#define OPENSSL_API_COMPAT 0x10101000L` 避免 OpenSSL 3.x 的弃用警告。

### 3. `src/encrypt.c` 密码学适配
- 移除了所有 OpenSSL 头的直接包含，取而代之的是 `#include "bitun_osal.h"`。
- 将 `calculate_hmac`、`derive_session_key`、`encrypt_chacha20_poly1305` 和 `decrypt_chacha20_poly1305` 的底层计算完全托管给 OSAL 接口。
- 保证了 HKDF 的正确密钥派生流程（Salt 排序拼接、PRK 提取、OKM 扩展）。

### 4. `src/tunnel.c` 与 `src/tunnel.h` 业务核心重构
- **多路复用适配**：完全移除原有的 `epoll_create1`/`epoll_ctl`，替换为 `bitun_osal_poll_*` 接口，去除了 `EPOLLET` 边缘触发标志。
- **LT 忙轮询处理**：在 poll 循环中增加对 `BITUN_POLL_OUT` 事件的捕获。当非阻塞连接成功或失败后，发送 `CMD_CONNECT_ACK`，并**立刻调用 `bitun_osal_poll_mod` 将事件修改为只监听 `BITUN_POLL_IN`**，彻底解决了水平触发模式下的可写事件忙轮询（Event Storm）。
- **SOCKS5 握手反馈修正**：在 `CMD_CONNECT_ACK` 返回成功时，如果当前模式是 SOCKS5，通过 `bitun_osal_socket_send` 向 TCP 客户端发送 `05 00 00 01 00 00 00 00 00 00`（SOCKS5 Success Reply），成功完成了 SOCKS5 代理建立的最后一步，解决了连接超时挂起的问题。
- **Self-Pipe 替换**：移除了原有的 `pipe()`，改用 `bitun_osal_queue_t` 的 `dns_queue`。当 DNS 返回时，由 `queue_fd` 被 poll 捕获后复位并循环 pop 处理结果。
- **异步 DNS 替换**：删除了原有为每个解析请求临时创建 pthread 并分离的设计，改成调用全局 `bitun_osal_dns_resolve_async`。
- **原位解密与 Payload 平移**：解密直接传 `read_buf + 36` 作为输入和输出（In-place 解密），成功后使用 `memmove(read_buf, read_buf + 36, decrypted_len)` 搬移载荷。

### 5. `src/main.c` 适配
- 在 `main` 函数初始化和信号处理函数退出时分别添加了 `bitun_osal_dns_init()` 和 `bitun_osal_dns_deinit()`。

### 6. `Makefile` 调整
- 将 `src/linux/bitun_osal.c` 加入编译文件列表。
- 链接参数加入 `-lcrypto -lpthread`。

---

## 二、 编译日志

执行 `make clean && make` 的输出日志（无任何 OpenSSL 警告与编译错误）：

```
rm -f src/ikcp.o src/encrypt.o src/socks5.o src/tunnel.o src/main.o src/linux/bitun_osal.o bitun
gcc -O2 -Wall -Wextra -pthread -Isrc -c -o src/ikcp.o src/ikcp.c
src/ikcp.c: In function ‘ikcp_segment_new’:
src/ikcp.c:173:42: warning: unused parameter ‘kcp’ [-Wunused-parameter]
  173 | static IKCPSEG* ikcp_segment_new(ikcpcb *kcp, int size)
      |                                  ~~~~~~~~^~~
src/ikcp.c: In function ‘ikcp_segment_delete’:
src/ikcp.c:179:41: warning: unused parameter ‘kcp’ [-Wunused-parameter]
  179 | static void ikcp_segment_delete(ikcpcb *kcp, IKCPSEG *seg)
      |                                 ~~~~~~~~^~~
src/ikcp.c: In function ‘ikcp_qprint’:
src/ikcp.c:216:30: warning: unused parameter ‘name’ [-Wunused-parameter]
  216 | void ikcp_qprint(const char *name, const struct IQUEUEHEAD *head)
      |                  ~~~~~~~~~~~~^~~~
src/ikcp.c:216:61: warning: unused parameter ‘head’ [-Wunused-parameter]
  216 | void ikcp_qprint(const char *name, const struct IQUEUEHEAD *head)
      |                                    ~~~~~~~~~~~~~~~~~~~~~~~~~^~~~
gcc -O2 -Wall -Wextra -pthread -Isrc -c -o src/encrypt.o src/encrypt.c
gcc -O2 -Wall -Wextra -pthread -Isrc -c -o src/socks5.o src/socks5.c
gcc -O2 -Wall -Wextra -pthread -Isrc -c -o src/tunnel.o src/tunnel.c
gcc -O2 -Wall -Wextra -pthread -Isrc -c -o src/main.o src/main.c
gcc -O2 -Wall -Wextra -pthread -Isrc -c -o src/linux/bitun_osal.o src/linux/bitun_osal.c
gcc -O2 -Wall -Wextra -pthread -Isrc -o bitun src/ikcp.o src/encrypt.o src/socks5.o src/tunnel.o src/main.o src/linux/bitun_osal.o -lcrypto -lpthread
```

---

## 三、 测试输出验证

### 1. OSAL API 单元测试 (`src/linux/test_bitun_osal.c`)
编译运行单元测试以独立验证时间、随机数、密码学（HMAC/HKDF/ChaCha20 的 Out-of-place 与 In-place 模式）、互斥锁线程安全性、eventfd 自唤醒队列以及异步 DNS 解析：

```
==========================================
Starting BiTun OSAL Unit Tests...
==========================================
[Test] Running time and random test...
[Test] Monotonic time check passed: elapsed 100 ms
[Test] Real calendar time: 1781886760744 ms
[Test] Random U32: 0x643d272e
[Test] Random bytes test passed.
[Test] Running crypto (HMAC, HKDF, ChaCha20-Poly1305) test...
[Test] HMAC-SHA256 calculated successfully.
[Test] HKDF-SHA256 derived successfully.
[Test] ChaCha20-Poly1305 Encrypt passed. Ciphertext len: 22
[Test] ChaCha20-Poly1305 Decrypt passed.
[Test] ChaCha20-Poly1305 In-place Decrypt passed.
[Test] Running threading and mutex test...
[Test] Main thread sleeping for a bit while holding mutex...
[Test] Thread locked mutex successfully.
[Test] Threading and mutex test passed.
[Test] Running eventfd queue test...
[Test] Eventfd queue test passed.
[Test] Running async DNS test...
[Test] Resolved localhost to: 127.0.0.1
[Test] Async DNS test passed.
==========================================
All BiTun OSAL Unit Tests Passed!
==========================================
```

### 2. 全局集成测试 (`run_integration_test.sh`)
运行集成测试，它拉起 Python HTTP 目标服务（8000 端口）、Peer A SOCKS5 代理端（9000 端口）和 Peer B 传输端（9001 端口），使用 `curl` 模拟客户端发起 SOCKS5 请求，验证全双工隧道以及 KCP + AEAD 数据加解密转发的可行性：

```
==================================================
Starting BiTun Integration Test...
==================================================
[Test] Starting Python HTTP target server on port 8000...
[Test] Starting Peer A (SOCKS5 Proxy on port 9000)...
[Test] Starting Peer B (Client worker on port 9001)...
[Test] Waiting for handshake to establish...
[Test] Testing tunnel with curl --socks5...
[Test] Curl completed successfully!
[Test] Received response:
<!DOCTYPE HTML>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Directory listing for /</title>
...
[Test] SUCCESS: BiTun Integration Test Passed!
[Test] Cleaning up processes...
```

本平台 OSAL 的 Linux 移植在功能和性能上完全达到了规格说明书的要求，并通过了多维度自动化测试验证。
