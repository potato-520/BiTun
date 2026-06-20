# BiTun 极简双向 SOCKS5 瑞士军刀重构提测与验证报告

本报告记录了对 BiTun 核心和接口进行重构，移除全部静态转发模式（`-L` / `-R`）并精简为纯粹双向 SOCKS5 代理特性的全过程与验证结果。

---

## 1. 变更范围与源码修改清单

我们对以下文件进行了重构与精简：

### 1.1 `src/tunnel.h`
* **移除冗余的映射模式枚举**：删除了原有的 `mapping_mode_t` 定义（包括 `MODE_SOCKS5`、`MODE_FORWARD_L`、`MODE_FORWARD_R`）。
* **简化配置结构体**：从 `tunnel_config_t` 中移除了 `target_ip`、`target_port` 和 `mode` 三个字段。重构后的结构体如下：
  ```c
  typedef struct {
      char *local_ip;
      int local_port;
      char *remote_ip;
      int remote_port;
      uint8_t psk[PSK_LEN];
  } tunnel_config_t;
  ```

### 1.2 `src/tunnel.c`
* **去除模式分支逻辑**：移除了所有 `config->mode == MODE_FORWARD_L` 或 `MODE_FORWARD_R` 的分支处理。
* **默认开启 TCP SOCKS5 监听**：只要配置了 `local_port > 0`，就无条件启动本地 TCP 监听端口，并始终将连接初始化为 SOCKS5 握手信道（`ch->socks5_handshake_done = 0`，调用 `socks5_init`）。
* **简化 `CMD_CONNECT` 的地址解析**：移除了接收端对 `ADDR_TYPE_SOCKS5` 的处理，对端接收到 `CMD_CONNECT` 时，总是将流连接直接投递到解析出的目标 IP/Domain 端口。

### 1.3 `src/linux/main.c`
* **简化命令行参数解析**：去除了 `-m`/`--mode` 和 `-t`/`--target` 参数解析以及相关的空值校验逻辑。
* **更新 Usage 及运行示例**：删除了 `-L` / `-R` 静态端口转发说明，只展示纯粹的 SOCKS5 对称打洞案例。

### 1.4 `run_integration_test.sh`
* **去除 `-m` 和 `-t` 参数**：重构了测试脚本以运行简化的 `bitun` 可执行程序。
* **测试双向代理连通性**：
  * **测试 1**：客户端 -> Peer A（SOCKS5 监听于 9000）-> 经过 KCP 隧道 -> Peer B（流量出口）-> 目标 HTTP 服务器（8000），通过 curl 验证返回内容；
  * **测试 2**：客户端 -> Peer B（SOCKS5 监听于 9001）-> 经过 KCP 隧道 -> Peer A（流量出口）-> 目标 HTTP 服务器（8000），验证双向代理均能完全跑通。

### 1.5 跨平台多语言文档（`README.md`, `docs/README.en.md`, `docs/README.ja.md`）
* 删除了所有静态转发（`-L` / `-R`）的描述、时序拓扑图以及示例，重新定义为“最纯粹、完全对称的加密双向 SOCKS5 代理网关”。

---

## 2. 编译与自动化测试结果

### 2.1 Linux 平台编译与集成测试
* **编译行为**：通过 `make clean && make` 成功编译出 Linux 版本的 `bitun` 二进制，无警告中止。
* **测试结果**：运行 `./run_integration_test.sh` 时，Peer A 和 Peer B 之间的 KCP 安全隧道握手成功，且**两个方向**的 curl 代理流量均在几毫秒内成功响应：
  * Peer A 出口 SOCKS5 连通：**成功 (SUCCESS)**
  * Peer B 出口 SOCKS5 连通：**成功 (SUCCESS)**

### 2.2 ESP32 交叉编译
* **编译行为**：运行 `cd src/esp32 && ./build.sh`
* **编译输出**：
  * 主静态库 [src/esp32/build/esp-idf/main/libmain.a](file:///home/chenming/BiTun/src/esp32/build/esp-idf/main/libmain.a) 重新成功链入生成。
  * 固件二进制包 `src/esp32/build/bitun_esp32.bin` 顺利通过链接，无任何阻塞性接口错误。
