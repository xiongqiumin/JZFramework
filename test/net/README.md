# JZFramework 网络模块测试

本目录包含 JZNet（TCP/UDP/WebSocket/MQTT/HTTP）网络模块的测试程序。所有测试都使用框架自身的
`JZTcpSocket` / `JZUdpSocket` / `JZWebSocket` / `JZMqtt` / `JZHttpServer` / `JZNetworkAccessManager`
实现，不依赖 Qt。

## 目录结构

| 文件 | 说明 |
|---|---|
| `test_net.cpp` | 综合测试入口，逐个运行 TCP/UDP/WebSocket/MQTT/HTTP/JZNetServer |
| `test_tcp.cpp` | TCP 单线程 + 多线程回环测试 |
| `test_udp.cpp` | UDP 单线程 + 多并发回环测试 |
| `test_webSocket.cpp` | WebSocket 单线程 + 多线程回环测试 |
| `test_mqtt.cpp` | MQTT 单线程 + 多线程回环测试 |
| `test_http.cpp` | HTTP 单线程 + 多线程回显测试（JZNetworkAccessManager） |
| `test_netServer.cpp` | JZNetServer 高层服务测试 |
| `net_test_common.h/.cpp` | 公共工具（TPS 计数、参数、退出看门狗、错误判定） |
| **`test_tcp_bench.cpp`** | TCP/UDP 上下行吞吐压测（独立程序，配合 iperf3 对测） |

---

## 一、综合自检测试（`test_net`）

运行所有网络模块的收发正确性 + 吞吐 benchmark。任一模块失败会立即 `exit(1)`。

### 构建
```bash
# 在 JZFramework 顶层 build 目录（已配置 Release）
cmake --build build --target test_net -j4
# 产物: build/test/net/test_net
```

### 三种运行模式（`--mode`）

**local（默认）：单机回环**，本进程同时起服务器和客户端自测
```bash
./test_net --mode local
```

**server（纯服务器）**：只起服务器，等待外部客户端连接（最多 30s）
```bash
./test_net --mode server --server <本机IP>
```

**client（纯客户端）**：连接外部服务器（延迟 10s 等服务器就绪）
```bash
./test_net --mode client --server <服务器IP>
```

> 跨机器对测时：A 机 `--mode server`，B 机 `--mode client --server A的IP`。
> `--server` 指定客户端连接的目标主机（默认 `127.0.0.1`）。

### 覆盖模块与端口

| 模块 | 测试内容 | 端口 |
|---|---|---|
| TCP | 单线程回环 + 多线程回环，序号校验 | 18100 / 18105 |
| UDP | 单线程 + 8 并发回环，序号/去重校验 | 18106 起 |
| WebSocket | 单线程 + 多线程文本消息回环 | 18000 / 18001 |
| MQTT | 单线程 + 多线程 publish/subscribe 回环 | 18103 / 18107 |
| HTTP | 单线程 + 多线程 GET 回显（JZNetworkAccessManager） | 内部指定 |
| JZNetServer | 高层服务端收发 | 内部指定 |

每模块结束都会用 `NetTpsCounter` 统计 TPS 并与阈值（默认 200）对比，不达标视为失败。

---

## 二、吞吐压测（`test_tcp_bench`，独立程序）

专门做 TCP / UDP **上下行吞吐压测**，可选 `up`(上行) / `down`(下行) / `bidir`(双向)，
支持**上下行限速**（默认 1Mb/s），可配合 Linux 压测工具 iperf3 交叉验证。

### 构建
```bash
cmake --build build --target test_tcp_bench -j4
# 产物: build/test/net/test_tcp_bench
```

### 命令行参数

| 参数 | 说明 | 默认 |
|---|---|---|
| `--mode up\|down\|bidir` | 上行/下行/双向 | bidir |
| `--host <ip>` | 对端地址 | 127.0.0.1 |
| `--port <p>` | 端口 | 18200 |
| `-t <sec>` / `--time` | 运行秒数 | 15 |
| `--size <bytes>` | 单块发送字节 | 65536 |
| `--interval <ms>` | 发送定时器间隔（非限速时） | 1 |
| `--rate <B/s>` | **上下行限速(字节/秒)，0=不限速** | **125000 (1Mb/s)** |
| `--proto raw\|iperf` | 裸数据 / iperf3 协议 | raw |
| `--transport tcp\|udp` | iperf 模式传输层 | tcp |

### 角色

**本程序始终作为 client（主动连接方）**，配合 Linux 压测工具（iperf3/nc 等）作为对端服务器。

### A. 裸数据压测（`--proto raw`，默认）

对端需一个能收发数据的服务器，可用 `iperf3 -s` 或 `nc -l`：
```bash
# 终端1: 对端服务器
iperf3 -s -p 18200

# 终端2: client 上行 / 下行 / 双向
./test_tcp_bench --mode up   --host 127.0.0.1 --port 18200 -t 15
./test_tcp_bench --mode down --host 127.0.0.1 --port 18200 -t 15
./test_tcp_bench --mode bidir --host 127.0.0.1 --port 18200 -t 15
```

### B. 配合 iperf3 对测（`--proto iperf`）

iperf3 作为标准对端，交叉验证 JZ 的收发吞吐。`test_tcp_bench` 实现了 **iperf3 协议握手**
（cookie + JSON 参数 + CREATE_STREAMS，控制连接和数据连接均用 JZTcpSocket），
因此 `iperf3 -s` 能直接识别并统计 JZ client 发送的数据。

#### B1. TCP
```bash
# TCP 上行（JZ client 发，iperf3 收）
iperf3 -s -p 18281
./test_tcp_bench --proto iperf --transport tcp --mode up --host 127.0.0.1 --port 18281 -t 15

# TCP 下行（iperf3 反向发，JZ client 收）
iperf3 -s -p 18282
./test_tcp_bench --proto iperf --transport tcp --mode down --host 127.0.0.1 --port 18282 -t 15

# TCP 双向
iperf3 -s -p 18283
./test_tcp_bench --proto iperf --transport tcp --mode bidir --host 127.0.0.1 --port 18283 -t 15
```

#### B2. UDP
```bash
# UDP 上行（JZ client 用 JZUdpSocket 发，iperf3 收）
iperf3 -s -p 18310
./test_tcp_bench --proto iperf --transport udp --mode up --host 127.0.0.1 --port 18310 -t 15 --size 1400

# UDP 下行（iperf3 反向发 UDP，JZ client 收）
iperf3 -s -p 18311
./test_tcp_bench --proto iperf --transport udp --mode down --host 127.0.0.1 --port 18311 -t 15 --size 1400
```
> UDP 的 `--size` 建议 ≤ 1470（数据报最大有效载荷，避免分片）。
> iperf3 server 不要加 `-u`（`-u` 是 client-only 参数）；server 通过 JSON 里的 `udp:true` 自动识别协议。

### C. 上下行限速（`--rate`，默认 1Mb/s）

框架级限速，两种传输层的机制不同：

| 传输层 | 限速实现 | 对端表现 |
|---|---|---|
| **TCP 下行** | `setReadLimit` 令牌桶 → net thread 停读(`is_full`) → 窗口收缩 | 对端**自动降速**，不丢包 |
| **UDP 下行** | `setReadLimit` 令牌桶 → `MG_EV_READ` 按数据报丢弃 | 对端全速发，**我们丢超额数据报** |
| **上行** | 应用侧 `RateBudget` 控制发送节奏 | 对端看到限速后的到达速率 |

```bash
# 默认即 1Mb/s：TCP 下行限速
iperf3 -s -p 18312
./test_tcp_bench --proto iperf --transport tcp --mode down --host 127.0.0.1 --port 18312 -t 15 --rate 125000

# 不限速
./test_tcp_bench --proto iperf --transport tcp --mode down --host 127.0.0.1 --port 18312 -t 15 --rate 0
```

---

## 三、HTTP 代理本地联调（`test_proxy` + `test_proxy_server.py`）

离线验证 `JZNetworkAccessManager` 的代理能力：

- **http 目标经代理转发**（请求行用绝对 URI）
- **https 目标经代理 CONNECT 隧道** + 隧道上 TLS 握手（核心验证点）

### 准备（本地一体化服务）

`test_proxy_server.py` 用 Python 一次性起三个服务，**无需任何外网**：

| 服务 | 地址 | 说明 |
|---|---|---|
| 目标 HTTP 站点 | `http://127.0.0.1:8080` | Flask，返回 `PROXY_TEST_OK` 可识别响应 |
| 目标 HTTPS 站点 | `https://127.0.0.1:8443` | Flask + 运行时生成的自签名证书（CN=127.0.0.1） |
| HTTP 代理 | `http://127.0.0.1:7001` | 原生 socket 实现，支持 CONNECT 隧道 |

> Flask (WSGI) 无法处理 CONNECT，故代理本体用标准库 `socket` 实现；Flask 只用于提供"被访问的目标站点"。
> 依赖：`flask`、`cryptography`（`pip install flask cryptography`）。证书每次运行自动生成，无需预置文件。

```bash
# 终端1: 启动本地代理 + 目标站点（Ctrl+C 退出）
python test/net/test_proxy_server.py

# 终端2: 构建并运行 C++ 代理测试
cmake --build build --target test_proxy --config Debug
./build/test/net/test_proxy            # 默认即走上面的本地地址
```

### `test_proxy` 目标地址（已默认改成本地）

`test_proxy.cpp` 默认目标已改为本地：

- 直连（无代理）：`http://127.0.0.1:8080`
- http 经代理：`http://127.0.0.1:8080`（经 `http://127.0.0.1:7001` 转发）
- https 经代理隧道：`https://127.0.0.1:8443`（经 `127.0.0.1:7001` CONNECT）

C++ 侧 `setSsl` 设了 `sslSkipVerify=true`，故自签名证书可被接受（仅验证隧道连通性）。
可用 `--http-target` / `--https-target` / `--proxy` / `--timeout` 覆盖。
退出码：`0`=全通过，`1`=有失败，`2`=参数/环境错误。

> 直连段与代理段使用**两个不同的** `JZNetworkAccessManager`，可顺带验证连接池
> `hostKey` 正确区分直连 / 代理转发 / https 代理隧道（不互相错误复用）。

---