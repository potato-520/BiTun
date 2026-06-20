# BiTun Cauchy-RS FEC 移植与自适应算法实现报告

本报告详细记录了在 BiTun 项目中实现基于 Cauchy 矩阵的 Reed-Solomon (RS) 前向纠错 (FEC) 算法、自适应 FEC 调节逻辑、数据包头部封装，以及接收端丢包统计反馈机制的设计与实现细节。

---

## 1. Cauchy-RS FEC 算法实现

我们新建了 [fec.h](file:///home/chenming/BiTun/src/fec.h) 与 [fec.c](file:///home/chenming/BiTun/src/fec.c)，实现了完整的 GF(256) 代数运算和 Cauchy 系统生成矩阵编解码器。

### 1.1 有限域 GF(256) 运算与初始化
* **生成多项式**：采用标准的 $x^8 + x^4 + x^3 + x^2 + 1$ (对应二进制 `0x11d`，十进制 `285`) 生成 GF(256) 指数表 (`gf_exp`) 和对数表 (`gf_log`)。
* **乘除法运算**：使用查表法优化 GF(256) 乘法 (`gf_mul`) 和除法 (`gf_div`)，消除运行时的动态取模计算，极大提升了嵌入式端 (ESP32) 的运行效率。
* **向量乘加优化**：实现了 `gf_mul_add_buf` 用于对数据缓冲区进行高效的有限域向量乘加操作。

### 1.2 Cauchy 系统生成矩阵构建
* 系统生成矩阵形式为：
  $$G = \begin{pmatrix} I_N \\ C \end{pmatrix}$$
  其中前 $N$ 行为单位矩阵 (Identity Matrix)，后 $R$ 行为柯西矩阵 (Cauchy Matrix)。
* 柯西矩阵 $C_{R \times N}$ 中第 $i$ 行、第 $j$ 列的系数定义为：
  $$C_{i, j} = \frac{1}{x_i \oplus y_j}$$
  我们选择 $x_i = i + N$，$y_j = j$。由于编码范围限制在 $N+R \le 24$ 内，集合 $\{x_i\}$ 与 $\{y_j\}$ 保证互斥且 $x_i \oplus y_j \ne 0$，除法运算永远安全有效。

### 1.3 RS 编码器设计
* 编码函数：
  ```c
  int fec_encode(uint8_t **data_blocks, int n, int r, size_t len, uint8_t **parity_blocks);
  ```
  直接将 $N$ 个数据块乘以 Cauchy 矩阵，计算出 $R$ 个冗余校验块，整个编码过程不涉及任何堆内存分配。

### 1.4 RS 解码器设计 (高斯-若尔当消元)
* 解码函数：
  ```c
  int fec_decode(uint8_t **received_blocks, int *received_indices, int num_received, int n, int r, size_t len, uint8_t **recovered_data);
  ```
* 接收端收集到任意 $N$ 个属于同一个 Group 的数据包或冗余包后，截取生成矩阵对应的 $N \times N$ 子矩阵 $A$。
* 使用 **Gauss-Jordan 消元法** 计算逆矩阵 $A^{-1}$，然后将接收到的向量乘以 $A^{-1}$，恢复出所有丢失的原始数据包。

---

## 2. 协议头部设计与数据包封装

### 2.1 6 字节 FEC 协议头结构
我们在 [fec.h](file:///home/chenming/BiTun/src/fec.h) 中定义了轻量级的 FEC 头部结构：
```c
typedef struct {
    uint16_t group_id;   // 组 ID
    uint8_t index;       // 包索引 (0 ~ N-1 为数据包, N ~ N+R-1 为冗余包)
    uint8_t n;           // 该组中的有效数据包数 N
    uint8_t r;           // 该组中的冗余包数 R
    uint8_t reserved;    // 保留字段，对齐到 6 字节
} __attribute__((packed)) fec_header_t;
```

### 2.2 数据块填充与定长处理
由于 FEC 要求编解码块大小相同，我们采用以下策略：
* 定义每个 FEC 块大小为固定 `1408` 字节 (`FEC_BLOCK_SIZE`)，支持承载最大 KCP MTU (1400 字节)。
* 每个数据块的前 `2` 字节以大端格式保存该包的真实长度 `len`，之后是真实的 KCP 载荷，剩余部分填 0。
* **数据包零复制/立即发送**：发送端在输出 KCP 数据包时，直接加上 6 字节 of FEC 头部后进行 ChaCha20-Poly1305 加密，并立即通过 UDP 发送，同时将该包内容拷入发送端 FEC 编码状态缓存中。
* **冗余包生成**：当发送端累积了 $N$ 个包，或者当前组的首包发送超过 `10ms` 时，发送端将当前组打包（若超时未满 $N$，则调整 $N_{effective} = data\_packet\_count$ 动态截断），计算出 $R$ 个冗余包并发送。这保证了极低的传输时延。

---

## 3. 丢包统计反馈与自适应 FEC 调节机制

### 3.1 控制指令定义
定义了新的控制指令：
```c
#define CMD_FEC_FEEDBACK   0x07
```

### 3.2 接收端丢包统计
* 接收端处理每个包时，统计已接收的分组包数以及这些包头部声明的预期总包数 ($N+R$)。
* 累计达到 `100` 个包或经过 `1` 秒时，计算当前的丢包率：
  $$\text{loss\_rate} = \frac{\text{expected\_packets} - \text{received\_packets}}{\text{expected\_packets}} \times 100\%$$
* 接收端将计算得出的丢包率 (单字节百分比，0-100) 通过 KCP 控制帧发送 `CMD_FEC_FEEDBACK` 给发送端。

### 3.3 发送端自适应 FEC 调节逻辑
发送端收到接收端的 `CMD_FEC_FEEDBACK` 时，动态调整后继组的 $N$ 与 $R$ 参数：
* 丢包率 $= 0\%$：退化为 $R=0$ (直通模式，不生成冗余包，极大地节省带宽)
* 丢包率 $\in [1\%, 5\%]$：设置 $N=10$, $R=1$
* 丢包率 $\in (5\%, 10\%]$：设置 $N=8$, $R=2$
* 丢包率 $\in (10\%, 20\%]$：设置 $N=5$, $R=2$
* 丢包率 $> 20\%$：设置 $N=4$, $R=3$

---

## 4. 编译与测试验证

### 4.1 单元测试验证
我们在 [test_fec.c](file:///home/chenming/BiTun/src/linux/test_fec.c) 中实现了一个独立的单元测试：
* 随机生成 10 个原始数据块，使用 `fec_encode` 生成 4 个校验块。
* 模拟丢弃 4 个原始数据块。
* 使用 `fec_decode` 对剩余 of 6 个原始块与 4 个校验块执行高斯消元还原。
* **测试结果**：100% 成功还原，所有恢复的数据块与原始数据完美匹配。

### 4.2 Linux 编译与集成测试
* 执行 `make clean && make`，Linux 版本顺利编译通过，二进制程序包含新模块。
* 运行 `bash run_integration_test.sh` 启动双端隧道和 Python HTTP 测试服务器，执行双向 SOCKS5 代理请求测试，**测试成功通过**。

### 4.3 ESP32 交叉编译验证
* 修改了 `src/esp32/main/CMakeLists.txt` 以引入 `fec.c` 组件。
* 执行 `bash src/esp32/build.sh` 启动 ESP-IDF 构建链。
* **构建结果**：
  ```
  Project build complete. To flash, run: idf.py flash
  ESP-IDF Build Successful! Main component library located at: build/esp-idf/main/libmain.a
  ```
  ESP32 固件编译链接完美成功，无任何错误。

---

## 5. 结论

本方案成功在 BiTun  symmetrical 隧道中融入了高性能、低延时的 Cauchy-RS 自适应前向纠错模块，保障了高丢包恶劣网络环境下的可靠性与吞吐量，且完美契合嵌入式 ESP32 的零动态分配内存约束。

报告完毕。
