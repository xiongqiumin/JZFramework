# Agent 控制面设计：server → cli → node

> **本文是这套架构唯一的文档**：目标与职责划分、协议、配置 schema、取舍、已知缺口与路线图。
> 部署/排障细节以各程序的 `--init` 生成物和 `--check` 输出为准（它们永远与代码同步），这里只保留设计与现状。
>
> 状态：P0（node `link` + cli `nodes[]` + `remote_ssh`）、P1（模式互斥）、P2（控制通道）、多队列、任务历史
> 均已实现；**P3（日志回传等）与 node 自述（§6.6）未实现**。大包策略未定，见 §4.4。

---

## 1. 目标与硬约束

一句话：**server 是唯一对外入口与管理面，cli 是执行代理（也能独立跑），node 是可选远端执行代理，只以一个 `remote_ssh` 工具出现在模型面前。**

六条硬约束（来自需求，实现时不得偏离）：

| # | 约束 | 含义 |
| --- | --- | --- |
| 1 | cli 支持两种模式 | `standalone`（自带 HTTP）/ `managed`（受 server 控制） |
| 2 | 两模式**互斥** | `managed` 时**不起 HTTP**；反过来 `standalone` 时不上报 server |
| 3 | server↔cli 走 **JZNetClient** | 用框架现成的 `JZNetClient`/`JZNetServer` + 自定义包，不自造协议 |
| 4 | node **预配置、不动态** | 启动时按配置连好；运行期不增删节点；cli 侧暴露为**单个工具**（remote_ssh 语义） |
| 5 | **执行面默认本地** | cli 不挂 node 时自己执行（`terminal_*` + 文件工具），node 是可选旁路 |
| 6 | **node 有两种模式，但角色固定** | `mcp`（起 `JZMcpServer`，行为同现状）/ `link`（起 `JZNetServer`，**不起 MCP**）；两种模式 node 都是 **server**，cli 永远是 **client** |

### 1.1 node 是可选层

**主形态（默认）**：`server → cli ×N`，每台 cli 在**自己那台机器**上本地执行。
分布式执行由"多台 cli"实现，不需要 node。

**保留 node 的理由**：现实里存在"**目标机器不能访问网络 / 装不了 cli**"的场景
（产线设备、只读工控机、客户机器只允许放一个薄代理）。node 在这些位置是唯一可行的执行出口。

判断什么时候该用 node（不是"要不要"，是"哪里用"）：

| 判据 | 装 cli 就够 | 该用 node |
| --- | --- | --- |
| 目标机器能否跑本框架 exe | 能 | 不能（无运行时/系统版本不匹配/不允许装） |
| 目标机器能否访问外网 | 能（要连 LLM 端点） | **不能**（LLM 端点不可达，只能做执行） |
| 是否允许在目标机器上放"决策程序" | 允许 | 只允许放一个薄执行代理 |
| 隔离要求 | 进程隔离够 | 需要独立机器/独立账号做边界 |
| 机器数量 | 几台到几十台 | 大量只读/只执行节点，统一由一个 cli 驱动 |

**结论**：node 是 **optional**，但**保留实现与发布**。文档、配置、默认拓扑不能把它当必经层；
"控制面"（server↔cli）与"执行面"（本地 / node）**彼此正交**——managed 与 standalone 都可能本地执行，也都可能挂 node。

⚠ 一个必须提前说清的边界：**node 不需要外网**（它不连 LLM），但需要**被 cli 连上**（两种模式都是 server 角色，
要入站可达）。若目标机器连入站也不通（只有出站权限），得改用反连（`connect:"dial"`）——见 §6.1.3。

### 1.2 验证面：统一在 cli 跑

`runVerifyAsync` 用 `JZProcess` 在 **cli 本机** 起进程，`cwd = m_config.rootDir`。

**不引入 `verify_host`。** 验证跑在哪台机器，取决于验证代码怎么写：
读 cli 本地文件/本机进程的，本来就该在 cli；要看远端状态的，让验证命令自己够过去
（`curl` 打 node 上的服务、共享盘、调 node 暴露的接口）。

必须认下的两个后果：

| 场景 | 后果 |
| --- | --- |
| `verify_cmd` 读的是 **node 本地路径** | ❌ 永远失败（`No such file`）→ 模型一直改、一直撞，白烧轮次 |
| cli 本机恰好有**同名旧文件** | ⚠ **假通过**（比假失败危险得多）：任务没做成却判 `success`，直接污染通过率 |

⇒ 写验证命令的硬要求：**验证读的那份东西，必须是本次任务真的产出的那份**。
路径要能唯一定位到产物；跨机时用网络接口，或用带 `taskId`/随机串的唯一名，
别让它能撞上本机旧文件。机器可执行的约束尚未做，见 §13。

### 1.3 明确不做

- ⛔ 不做运行期动态加/删 node（不搞"注册中心发现节点"）；
- ⛔ 不做 server 侧的 agent 自动扩缩容 / 任务队列持久化（只做"在线就派、离线就拒"）；
- ⛔ 不在 node 上发明第二套协议（node 是执行面，不是控制面参与者）；
- ⛔ 不在 cli 里加"多轮调度"（跑几轮/何时停归提交方，是既有约定）；
- ⛔ cli 不承担端口暴露/打洞（那属于网络层：WireGuard / Tailscale / `ssh -L` / 独立 frpc 进程）；
- ⛔ 不做 `verify_host`（理由见 §1.2）。

---

## 2. 三个平面

| 平面 | 协议 | 承载 | 谁发起 |
| --- | --- | --- | --- |
| **控制面** | JZNet（TCP + 握手 token + ChaCha20） | 注册 / 心跳 / 任务下发 / 取消 / 事件回传 | **cli → server（出站，穿透 NAT）** |
| **任务面** | 同上（packet）或 cli 本地 HTTP（仅 standalone） | 提交一次任务、拿一次回包 | 提交方 / server |
| **执行面（本地，默认）** | 进程内 `JZVirtualTerminal` + 文件工具 | `terminal_*` / `read_file`… | cli 自己 |
| **执行面（远端，可选）** | **JZNet（`NodePack_exec`）**：node `link` 模式起 `JZNetServer`，cli 用 `JZNetClient` 连 | `remote_ssh` 一条命令 | **cli → node** |

关键点：**控制面由 cli 主动连 server，执行面也由 cli 主动连 node** —— 整条链路上 cli 是唯一的 client，
server 与 node 都是 server 角色。这样内网机器不需要 frp、不需要端口映射，两端也不必知道 cli 的地址。

---

## 3. 两种运行模式

```
(A) standalone
   提交方/浏览器 ──HTTP──► cli :8080 ──► JZAgent ──► 本地工具 / remote_ssh(node)

(B) managed
   server :8088（管理台/API，默认 HTTPS + 客户端证书）
        ▲  JZNetClient 出站连接（token）
        │
      cli（无 HTTP）──► JZAgent ──► 本地工具 / remote_ssh(node)
```

### 3.1 模式判定

配置驱动，**不由命令行开关决定**（与"命令行只有 `-c/--init/--check/-h`"的既有约定一致）：

```
server.host 非空  → managed
server.host 为空  → standalone
```

### 3.2 启动分支（`binary/agent_cli/main.cpp`）

```cpp
JZAgentCli cli;
if (!cli.init(config, err)) return 1;

cli.startNodes();        // 两种模式都要：按 nodes[] 建 JZNetClient 连 link node，挂 remote_ssh 工具
                         //   nodes[] 为空 ⇒ 什么都不做，工具表里没有 remote_ssh

if (config.managed())
    cli.startServerLink();   // JZNetClient 连 server；不建 JZHttpServer
else
    cli.startHttp();         // POST /task + 网页控制台
return app.exec();
```

### 3.3 互斥规则表

| 能力 | standalone | managed |
| --- | --- | --- |
| `JZHttpServer`（`/task` `/info` `/events` `/reset` `/terminal*`） | 起 | **不起** |
| 网页控制台 `http/` 静态资源 | 起 | **不起** |
| `JZNetClient`（连 server） | 不连 | 必连 |
| 任务来源 | 外部 `POST /task` | server 的 `taskSubmit` |
| 取消/重置 | `POST /reset` | `taskCancel` / `reset` 包 |
| 诊断 | 网页 + curl | **只能靠 server 侧视图**（见 §13） |

⚠ 这条最容易漏：`POST /reset` 是长程装置/评测脚本杀任务的正规手段（`agent_cli.py` 超时就靠它）。
managed 模式下必须有等价的 `taskCancel` 包，否则任务没法中断。

---

## 4. 控制通道协议

### 4.1 传输

复用 `JZNetClient` / `JZNetServer`（`JZNet/JZNetClient.h`、`JZNet/JZNetServer.h`）：

- 客户端：`setToken()` → `connectToHost(ip, port)` → `sigConnected` → `sendPacket()` / `sigPacketReceive`；
- 服务端：`setToken()` → `listen(port)` → `sigAccept` + `nextPendingConnection()` → `sigPacketReceive(connId, pack)` / `sendPacket(connId, pack)`；
- 握手由框架内部完成：`JZNetPackHand(nonce)` → `JZNetPackHandRes`，双方用 `"JZ" + nonce + token` 派生 ChaCha20 密钥，
  **之后的包都是加密的**（见 `JZNet/JZNetClient.cpp:194-278`）。

⚠ **token 的校验语义要清楚**：握手只校验"客户端带没带 nonce"与服务端"有没有 token"是否一致
（`hand->nonce.empty() != m_token.empty()`），**不比对 token 内容**。token 写错的表现不是明确报错，
而是密钥不同 → 后续包解不开 → 连接被关；token 为空则整条链路不加密、也不鉴权。所以：

- 协议里必须有应用层 `hello` / `helloAck`，**3 秒内没收到 `hello_ack` 就判定 token 错误**并给出明确日志；
- 错误信息别只说"连接断开"，要提示"token 不匹配"这个最可能的原因；
- `hub.token` 与 node 的 `link` token 都必须非空（node 侧已硬性拒绝启动，server 侧见 §13）。

### 4.2 包类型

集中在一个头文件（仿 `JZFrpPacket.h` + `JZFrpPacketInit()`）。两套包号不冲突：

- node 执行面 `NodePack_*`：`NetPack_user + 1` 起（1000…）；
- server↔cli 控制面 `HubPack_*`：`NetPack_user + 101` 起（1101…）。

```cpp
enum {
    HubPack_hello = NetPack_user + 101,  // cli -> server
    HubPack_helloAck,                    // server -> cli
    HubPack_heartbeat,                   // cli -> server
    HubPack_taskSubmit,                  // server -> cli
    HubPack_taskCancel,                  // server -> cli
    HubPack_reset,                       // server -> cli
    HubPack_taskEvent,                   // cli -> server
    HubPack_taskEnd,                     // cli -> server
};
void JZAgentHubPacketInit();   // 两边启动时各调一次（在 JZNetPackInit 之后）
```

> `logAppend`（日志回传）属于 P3，**包与实现都还没有**，见 §13。

⚠ 注册时机：必须在 `JZNetPackInit()`（`JZApplication` 构造时已调用）**之后**
`JZNetPackManager::instance()->registPack<T>(type)`，否则收到的是 "unknown packet"。

### 4.3 消息表

| 方向 | 包 | 字段 | 说明 |
| --- | --- | --- | --- |
| cli→server | `hello` | `agentId, name, version, rootDir, modelName, nodes`（JSON 数组字符串） | 上线自报 + 节点清单 |
| server→cli | `helloAck` | `ok, error, acceptTasks, serverTimeMs` | `ok=false` 断开重连；`acceptTasks=false` 时只上报不接任务（维护模式） |
| cli→server | `heartbeat` | `busy, round, taskId, nodes` | 5s 一次；**节点上下线/忙闲靠它刷新**，不额外发包 |
| server→cli | `taskSubmit` | `taskId` + `/task` 的全部字段（JSON 字符串） | 语义与 `POST /task` **完全一致**，cli 直接复用 `submitTask()` |
| server→cli | `taskCancel` | `taskId` | 只取消**指定**任务（排队或在跑）；取消全部用 `reset{scope:"task"}` |
| server→cli | `reset` | `scope`（`task` / `history`） | `task` = 取消全部任务；`history` = 只清对话历史 |
| cli→server | `taskEvent` | `taskId, type, data` | **`type` 直接复用现有 SSE 的取值**：`thinking/reply/tool_call/tool_result/verify/error/task_start/task_end` |
| cli→server | `taskEnd` | `taskId, result`（JSON） | 一次任务一次，务必送达（见 §5.3） |

**不做第二套事件语义**：模型在干什么（think / tool_call / verify）这套词已经在 SSE 里定死了，
packet 里换名字只会让两套 UI 对不上。

### 4.4 大小与截断

- 单包上限 **256KB**（`MG_MAX_PACK_SIZE`，`JZNet/JZNetPacket.h`）；
- **不做分片**：重组要状态机 + 超时，而收益只覆盖"想一次看几 MB 日志"这种 UI 和模型都不需要的场景。
  要全文就先 `tail`/重定向到文件再分段读（模型侧做法）。
- 上行（cli→server，`JZHubLink` 里的 `capText`）：
  - `taskEvent.data` 超 **96KB**、`taskEnd.result` 超 **160KB** ⇒ 换成
    `{truncated:true, bytes, preview:"<头 4KB>"}`；
  - **不硬切 JSON**（切坏会让 server 解析失败）；
  - 余量是给包头与键名留的，数值远低于 256KB。
- 下行（server→cli）`taskSubmit` 目前**不裁**：`images` 里的 dataURL 可能把单包顶爆。
  这是已知风险，处理方式未定，见 §13。

### 4.5 心跳、重连、离线

| 参数 | 值 | 说明 |
| --- | --- | --- |
| 心跳间隔 | 5s | cli 主动发 `heartbeat`；忙闲变化时立刻多发一次 |
| 判离线 | 20s | server 每 5s 扫描，超过 4 个周期没收到即置 `offline`，管理台灰色 |
| 重连退避 | 1/2/4/8/…/30s 封顶 | cli 侧 `sigDisconnected` 后按序重试，成功后清零 |
| 重连补发 | `hello` + 最新状态快照（`busy/round/taskId/nodes`） | 让 server 重建视图 |

⚠ 判定必须靠心跳而不是 TCP 断开：拔网线/休眠时 socket 仍"通"，断开信号等不到。

---

## 5. 任务生命周期

### 5.1 正常路径

```
提交方 ──POST /api/tasks{agentId, ...}──► server
                                          │ 选 agentId（离线 → 404；忙闲交给 cli 队列）
                                          ▼
                                    taskSubmit ──► cli
                                                   │ submitTask()（队列调度）
                                                   │ ├─ taskEvent(thinking/reply/tool_call/tool_result/verify)…
                                                   ▼
                                              taskEnd ──► server（归档）
                                                             │
提交方 ◄────────────── 200（同步等待式）／或先返 202 + 轮询任务记录 ───────────────┘
```

- **任务在 cli 里跑**：LLM 调用、验证循环、重试预算、长期记忆全不动；server 只做转发与聚合。
- server 的 `/api/tasks` 两种回包方式：`wait:true`（默认，等 `taskEnd` 回 200），
  `wait:false` 先回 `202 {taskId}` 再轮询任务记录。两种方式都登记同一个超时与取消生命周期；网页使用后者。
- server 侧超时 `task_timeout_ms`（默认 900000 ms）：先发 `taskCancel`，同步提交方收 `504`，异步只落历史。

`taskSubmit` 就是现有 `POST /task` 的请求体，`taskEnd` 就是它的回包（语义完全一致）：

| 请求字段 | 说明 |
| --- | --- |
| `prompt` | 任务文本（与 `images` 至少一个非空） |
| `images` | dataURL / http 链接数组，或 OpenAI `messages` 里的 `image_url` |
| `verify_script` / `verify_cmd` | 验证脚本文件 / 直接命令（`verify_cmd` 优先） |
| `verify_timeout` / `max_verify_rounds` | 覆盖配置里的 `verify.timeoutMs` / `verify.maxRounds` |
| `max_retries` / `max_fix_retries` / `retry_delay_ms` | 覆盖 `llm.*` 的重试预算 |
| `use_memory` | `false` = 本次不注入长期记忆 |
| `wait` | 仅 server：`false` 立刻回 202 |

| 回包字段 | 说明 |
| --- | --- |
| `status` | `success` / `failed` / `cancelled` |
| `verify_rounds` / `retries`（`retries_transient` / `retries_fix`） | 轮次与重试统计 |
| `agent_reply` / `last_verify_output` / `error` | 结论、最后一次验证输出、失败原因 |

### 5.2 取消与重置

| 场景 | 消息 | cli 行为 |
| --- | --- | --- |
| 提交方主动取消 | `taskCancel{taskId}` | 只取消指定任务（排队或在跑，含验证、本地后台命令与对应远端 exec），不影响其他队列；回一次 `taskEnd{status:"cancelled"}` |
| 取消全部 | `reset{scope:"task"}` | 排队任务逐条回 `cancelled`，各队列各自收尾 |
| 清会话（换新任务前） | `reset{scope:"history"}` | 只清对话历史，不动在途任务 |
| server 判定卡死/超时 | `taskCancel` + 记审计 | 同第一行 |

取消收尾期间**禁止重入派发**（否则排队任务会在收尾途中有被派出去）。

### 5.3 断线与崩溃

| 情况 | 处理 |
| --- | --- |
| cli 与 server 断连、任务仍在跑 | 任务继续在 cli 本地跑；记录标 `lost`；同步提交方（还在等）回 502。当前不缓存、不重连补发 `taskEnd` |
| cli 进程被杀 | server 侧该连接断 → 标 offline；在途任务标 `lost`，不重试（重试由提交方决定） |
| server 重启 | cli 按退避重连；server 用 `hello` 重建视图；载入历史时把 `running` 一律改 `lost`，**不恢复**未完成任务（避免重复执行） |
| 任务提交时 agent 忙 | server 不拦截，交给 cli：有空闲队列就执行，全忙按 `task.maxPending` 排队，排满才拒绝 |

铁律：`timeout/lost/cancelled` 等终态一旦形成，迟到的 `taskEnd` 不得覆盖。

---

## 6. node（`mcp` / `link` 两种模式）与 `remote_ssh`

### 6.1 定位

node 是**可选的远端执行代理**（§1.1）：装在目标机器上，不做决策、不连 LLM。
`link` 模式下它只做"执行一条命令 + 权限审核"；`mcp` 模式下它就是现成的被控端 MCP server。
**不做运行期增删**：node 列表在 cli 启动时从配置读入并连好，运行期只允许"连不上 → 标 offline"。

两条与"必要性"直接相关的规则：

- `nodes[]` 为空 ⇒ 纯本地执行，**工具表里不出现 `remote_ssh`**（不注册空工具、不给模型一个永远报错的入口）；
- 挂了 node 之后，本地工具**照旧存在**——本地与远端是同一个工具的两个目标，不是二选一。
  这也是"本地终端 + ssh"的自然模型，不需要再造概念。

#### 6.1.1 验证在哪跑（统一在 cli）

| 任务执行在 | `verify_cmd` 在哪跑 | 结果 |
| --- | --- | --- |
| local（默认） | cli 本地（`runVerifyAsync`，`cwd=rootDir`） | 正常 |
| node，验证只需本机/网络可达 | cli 本地 | 正常 |
| node，验证要读 node 的**本地路径** | cli 本地 | ❌ 一直 `No such file` |

- ✅ 成立：验证检查本机东西，或经网络访问远端（HTTP / DB / 共享盘）；
- ❌ 不成立：验证直接读 node 上的本地路径；
- ⚠ 更危险：cli 上有陈旧同名文件 → **假通过**（§1.2）。

同理，`verify_script` 一直是"cli 工作区里选一个脚本"，跨机时它同样只在 cli 侧有意义。

#### 6.1.2 node 的两种模式

**角色是固定的：node 永远是 server，cli 永远是 client。**

| 模式 | 起什么（server 角色） | 谁连它（client 角色） | 用途 |
| --- | --- | --- | --- |
| **`mcp`（默认，单独用）** | `JZMcpServer::listenHttp` ⇒ `/mcp`，工具面**与现状一致**（`terminal_*` + 文件工具） | `code_agent` / 任意 MCP 客户端 | 独立被控端，不改现有用法 |
| **`link`（配合 cli）** | **不起 MCP**；起 `JZNetServer` + token，只处理 exec 包 | cli 的 `JZNetClient` | `remote_ssh` 的执行后端 |

配置 `mode: "mcp" | "link"`，默认 `mcp`（现有部署升级后行为不变）。两种模式**只有一个在跑**，不会同时监听两个端口。

整条链路上只有 cli 是 client，另两端都是 server，这是刻意的对称：

```
   server（JZNetServer）  ◄── JZNetClient ── cli ── JZNetClient ──►  node（JZNetServer）
                     控制面                             执行面
   node（JZMcpServer）    ◄── MCP client ── code_agent / 第三方       ← mcp 模式时才存在
```

- 好处一：cli 只需要"会连"，两端都不需要知道对方的地址（server 不用配 cli 的 IP，node 也不用配 cli 的 IP）；
- 好处二：`link` 与 `mcp` 对 node 而言只是"换一个 server 实现"，审核/执行那套代码完全共用。

两条硬规则：

- `link` 模式**没有 tools/list、没有 MCP 断连语义**，只有一个隐含的 `exec`；
- **"多节点"这件事只在 cli 侧存在**：cli 连多少个 node 由 `nodes[]` 决定，node 自己不知道有兄弟节点。
  这正是放弃"每个 node 一个 MCP server"的原因 —— 多台 MCP server 在模型侧会变成
  `node1__run` / `node2__run` 一堆名字，还得让模型自己挑；收到一个 `remote_ssh(host=...)` 后
  路由完全在 cli 内部完成，模型只看见一台"虚拟的 ssh"。

#### 6.1.3 连通性（node 能不能放在无网机器上）

| 要求 | mcp 模式 | link 模式 |
| --- | --- | --- |
| **入站可达** | 必须（cli/客户端 `POST /mcp`） | **必须**（cli 连 node 的 `JZNetServer`） |
| 出站外网 | 不需要 | 不需要 |
| 出站内网 | 不需要 | 不需要 |

**node 全程不需要外网**（不连 LLM、不下载），这正是它能放在无网设备上的理由；它只需要被 cli 连上。

⚠ 边界情况：目标机器连**入站**都封了（只允许出站）⇒ 两种模式都不成立。此时要加 `connect: "dial"`：
node 用 `JZNetClient` 主动连 cli（或 server）的 `JZNetServer`，cli 把这条连接同样包成 `remote_ssh`。
**第一版不做**，只在配置里预留字段名。

### 6.2 cli 侧只暴露一个工具

模型侧只看到一个工具（多节点靠 `host` 参数路由）：

```
remote_ssh(host?, command, cwd?, timeout_ms?)

> ⚠ 2026-11-11：`action=query/cancel`、`background`、`run_id`、`tail/grep/incremental` 均已删除。
> 现在只有"跑一条命令、同步回输出"；后台写 `&` + 重定向，之后 `cat` 日志。
> 本节其余内容是当时的协议设计记录（`NodePack_termQuery` 包还留着，节点只回一句"不再保留运行记录"）。
```

| 参数 | 说明 |
| --- | --- |
| `action` | `run`（默认）/ `query` / `cancel`；同一个工具按动作分发 |
| `host` | `nodes[]` 里的 `name`；空 = 唯一/默认节点 |
| `command` | **整条交给 shell**（`\|`、`>`、`&&`、通配符、`$VAR` 都生效）；`action=run` 必填 |
| `cwd` | 工作目录（相对路径按该 node 的会话目录解析）；节点侧走 `changeDir` 换目录，**不再拼进命令串**（拼串会把 `$()`/反引号带进 shell，绕过审核）。注意会话 cwd 是持久的 |
| `timeout_ms` | 前台超时，默认 120s，上限 15min（与本地 terminal 前台一致）；后台不设 |
| `background` | `run`：`true` = 节点**立刻回 `started`**（不等命令结束），之后用 `query` 取状态/日志。命令仍占着节点那唯一的前台终端 |
| `run_id` | 由执行结果给出，**带节点前缀**（`line1:r3`），据此路由回节点；`cancel` 必填，`query` **可省略 = 该节点最近一次运行**（与本地 `terminal_query` 同口径） |
| `tail` / `grep` / `incremental` | `query` 的过滤：最近 N 行 / 子串（不区分大小写）/ 只取上次之后新增 |

返回是**一段文本**（不是 JSON），模型直接读：

```
[node line1] run_id=line1:r1 exit=0 320ms  cwd=D:/line1
<stdout/stderr 合流输出>

[node line1] 已启动 run_id=line1:r2 (后台运行, 不设超时); 用 action=query 查日志/状态, action=cancel 停掉

[node line1] line1:r2 运行中 [后台]  累计 3 行
...
```

前台失败时首行变成 `失败(exit=1)` 或 `超时`，并附 `error` 原因。`query` 的游标推进在 node 侧
（`advanceRunReadPos`），所以 `incremental=true` 轮询不会把同一段日志反复灌进上下文。
**没有常驻 shell**，`cd`/`export` 不跨命令（与 node 现有语义一致，见 `JZAgentNode::buildInstructions`）。

### 6.3 为什么在 cli 侧收成一个工具

| 做法 | 模型看到 | 问题 |
| --- | --- | --- |
| 每个 node 一个 MCP server，工具直接挂进 agent | `node1__terminal_run`、`node2__terminal_open`… | 工具表爆炸；模型要自己挑节点；`terminal_open` 这类会话管理会被误用 |
| **cli 注册一个 `remote_ssh`，内部按 host 路由** | 1 个工具 | 节点对模型不可见，挑选由人（配置的 default）或 `host` 参数决定 |

`link` 模式下 node 只实现 `exec`，所以工具语义天然对齐；`mcp` 模式的工具面**保持不变**（给别的 MCP 客户端用）。

**代价（要认）**：`background` / `query` / `cancel` 已经有（走同一个工具的 `action`），但**没有流式**：
`terminal_run` 的 `stream=true` 靠 MCP 的逐块通知，JZNet 侧没接 `onChunk`，所以长命令只能
"后台起 + `action=query` 轮询"。另外前台单次回包的 output 仍按 120KB 头尾裁剪（全文在节点运行记录里，
用 `query` 取）。

### 6.4 cli→node 传输：JZNet（`NodePack_*`）

与 server↔cli 同一套框架（`JZNetClient`/`JZNetServer` + token 握手加密），只是包类型与端口不同：

| 方向 | 包 | 字段 |
| --- | --- | --- |
| cli→node | `NodePack_hello` | `clientId, cliVersion`（node 侧做日志/审计；不做路由） |
| cli→node | `NodePack_exec` | `runId, command, cwd, timeoutMs, background` |
| node→cli | `NodePack_execResult` | `runId, ok, exitCode, timedOut, durationMs, cwd, output, error, started`（`output` 为 stdout/stderr 合流；`started=true` 表示后台已启动、不是最终结果） |
| cli→node | `NodePack_execCancel` | `runId`（任务取消、reset、超时、或 `action=cancel`） |
| cli→node | `NodePack_termQuery` | `runId, tail, grep, incremental`（等价 `terminal_query`；过滤在节点侧做） |
| node→cli | `NodePack_termQueryResult` | `runId, found, running, ok, background, timedOut, exitCode, durationMs, totalLines, firstLine, readPos, text, error` |
| node→cli | `NodePack_helloAck` | `ok, error, nodeName, version, rootDir, shell`（完整自述设计见 §6.6，未实现） |

实现要点：

- **并发**：每个 node **同时只跑 1 条 exec**（单终端串行），cli 侧忙时直接拒绝、不排队；
  `maxConcurrent` 目前只是预留字段，配几都不改变（后台命令也占这唯一的前台终端）；
- **超时双保险**：cli 侧 timer 到点发 `execCancel` 并本地收口；node 侧也用 `timeoutMs` 兜底杀进程树；
- **任务取消**：`endLoop()` 按发起队列的 owner 取消对应 `remote_ssh`，不误停其他队列共享连接上的命令；
  node 侧只停 `activeRunId` 与 runId 映射一致的那条（旧 runId 一律忽略），避免误杀后来者；
- **多 cli**：`(连接, runId)` 才是节点的映射键 —— cli 的 runId 是它自己按"节点别名+序号"生成的，
  两台 cli 会重名（都是 `line1:r1`）。连接断开时清掉该连接的全部映射并停掉它在跑的命令；
- **会话状态**：link 模式用的是 `JZVirtualTerminal` 的有状态解释器（`cd` / `export` 跨命令保留），
  `cwd` 参数等价于先 `cd` 过去，之后一直生效；
- **大输出**：`execResult.output` 超过 **120KB** 时按"头 60KB + 显式提示 + 尾 60KB"裁剪（按 UTF-8 边界），
  确需全文就先 `tail`/重定向到文件再分段读，不在协议层做分片；
- **token**：node 的 `JZNetServer::setToken` 与 cli 的 `JZNetClient::setToken` 一致；token 错的表现是
  "包解不开/断连"，所以 cli 侧要把 `hello/helloAck` 3s 超时报成"token 或端口不对"；
- **重连**：`sigDisconnected` → 1/2/4/…/30s 退避重连；`nodes[]` 里标 offline，`remote_ssh` 调用直接报
  "节点 line1 离线"，**不要**悄悄改走本地（会让模型以为命令生效了）。

### 6.5 node 侧配置与权限

```json
{
  "nodeName": "line1", "mode": "link",
  "host": "0.0.0.0", "port": 7500, "token": "t-line1", "maxConcurrent": 1,
  "rootDir": "D:/line1", "trustedDirs": [],
  "allowTerminal": true, "allowFileTools": true,
  "allowPackageManager": false, "allowNetwork": false, "unrestricted": false,
  "shellProgram": "", "maxSessions": 8, "recycleSec": 900, "logFile": ""
}
```

| 字段 | 说明 |
| --- | --- |
| `mode` | `mcp`（默认，单独当 MCP server）/ `link`（**不起 MCP**，只起 `JZNetServer`） |
| `token` | 仅 `link`：握手口令，**必填**（空 = 拒绝启动） |
| `maxConcurrent` | 仅 `link`：**预留字段，当前版本不生效**。节点是一台终端串行（同时 1 条），后台命令也占这一条；要并行只能多开 node |

其余字段（区域、命令策略、shell、会话回收、日志）两种模式完全一致。设计上的两条约束：

- `allowTerminal/allowFileTools` 在 `link` 模式下不再影响"工具面"（没有 MCP 工具），但**仍作为执行策略**保留：
  `allowTerminal=false` 时 exec 一律拒绝；
- `rootDir/trustedDirs/unrestricted` 等区域与命令策略**完全复用** `JZVirtualTerminalConfig`，link 与 mcp
  两条路共用一套审核代码（`JZAgentNodeHost::refresh` 已经是这个结构，不需要为 link 写第二套）。

### 6.6 node 自述：`helloAck` 该带什么（设计，未实现）

**只回 `nodeName/version/rootDir/shell` 不够。** cli 和模型要回答三类问题，缺任何一类都会导致"明明连上了却一直撞墙"：

| 问题 | 缺了会怎样 |
| --- | --- |
| 连到的是**哪台**（身份） | 重装/改名/端口填错时 `nodeName` 可能相同，告警指错机器 |
| 这台**能干什么**（能力） | 禁网/禁装包的节点上模型反复试 `curl`/`pip`，每撞一次烧一轮 |
| 现在**什么状态**（运行态） | 刚重启（说明上次崩了）、盘快满、已在跑一条，全都看不见 |

#### 6.6.1 字段表

`NodePack_helloAck`（node→cli）。**不做兼容分支**：结构直接改，老 node 连新 cli 时靠 `protoVersion` 不匹配**明确拒绝上线**，
而不是让它落到"包解不开"这种最难查的表现上。

| 字段 | 类型 | 来源 | 给谁 | 为什么需要 |
| --- | --- | --- | --- | --- |
| `ok` / `error` | bool / string | 执行终端是否建好 | 人 | 上线成败与原因（"连不上"是最没用的报错） |
| `protoVersion` | int | 常量 | 人 | 不匹配 ⇒ 直接拒绝并报"cli/node 版本不一致，请同步升级" |
| `nodeName` | string | 配置 `nodeName` | 人 / 模型 | 路由仍按**配置里的 name**，不用自述名 |
| `version` | string | **构建时注入**（`JZ_BUILD_VERSION` 或 commit） | 人 | ⚠ 不再读配置 `version`：手写的 `1.0.0` 判断不出程序新旧 |
| `platform` | string | 编译期 `win-x64` / `linux-x64` / `linux-arm64` / `darwin-arm64` | 人 / 模型 | 决定命令写法（`bash` vs `powershell`）、路径分隔符 |
| `hostName` | string | `gethostname()` | 人 | 两台机器都叫 `line1` 时靠它区分；**只用于展示，不做路由** |
| `rootDir` | string | 规范化后的工作区 | 人 / 模型 | `cwd` 语义；等于 `execResult.cwd` 的根 |
| `shell` | string | 固定 `jz_shell` | 人 | 终端用**内建解释器**，不再探测外部 shell；命令写法按 POSIX sh 子集 |
| `trustedDirs` | int | `trustedDirs.size()` | 人 / 模型(数量) | 只给**数量**不给路径：不泄露客户目录结构，但模型知道还有别的可写区 |
| `allowTerminal` | bool | 配置 | 人 / 模型 | `false` ⇒ 这台什么都跑不了 |
| `allowPackageManager` | bool | 配置 | 模型 | 禁 ⇒ 别试 `pip`/`npm`/`apt` |
| `allowNetwork` | bool | 配置 | 模型 | 禁 ⇒ 别试 `curl`/`wget`/`git clone` |
| `unrestricted` | bool | 配置 | 人 | 高危：等于交出 shell，管理台必须标红 |
| `maxConcurrent` | int | 配置 | 模型 | 会不会直接回"正忙" |
| `running` | int | 运行态 | 人 / 模型 | 当前在跑几条 |
| `startTimeMs` | int64 | 进程启动时刻 | 人 | 刚重启 = 上次可能崩了（比 uptime 直观） |
| `diskFreeMB` | int64 | `rootDir` 所在盘，取不到填 `-1` | 人 / 模型 | 设备型节点最常见的失败原因 |

**刻意不传**：

- ❌ `token`、配置全文、环境变量、`trustedDirs` 的完整路径 —— 纯泄露面，对决策没帮助；
- ❌ 已装软件 / 包管理器探测清单 —— 慢且不可靠，要看就让模型自己跑一条 `which python`；
- ❌ 会话/终端列表 —— `link` 模式只有一台终端，没有"列表"这回事；
- ❌ `allowFileTools` —— 只有 `mcp` 模式有文件工具，`link` 模式下它不影响任何行为，不进自述。

#### 6.6.2 自述怎么进到模型眼里

`helloAck` 到了 cli 之后，**必须重设 `remote_ssh` 的工具描述**，否则这些字段只有人能看到。
框架这边是支持的：`JZAgent::buildTools()` 每次请求都重新调 `tool->toolDefine().toJson()`
（`JZAgent/JZAgent.cpp:628-634`），所以 `JZRemoteSshTool` 在 `helloAck` 后调一次 `setToolDefine()`，**下一轮 LLM 请求就生效**，
不需要重启、不需要重建 agent。

描述格式（每个节点一行，只写**限制**，全开时省略，避免描述变长）：

```
可用节点:
  - line1 (10.0.0.7:7500) [默认] linux-x64 bash /home/line1 并发1 禁装包/禁联网
  - line2 (10.0.0.8:7500)         win-x64  pwsh D:/line2   并发1
  - line3 (10.0.0.9:7500)         [离线]
```

三条硬规则：

1. **离线节点也要列**，标 `[离线]` —— 模型要能回答"为什么不能用 line3"；离线时**不填能力位**（不能猜）；
2. **能力位只写限制**（禁装包 / 禁联网 / 受限 / 并发N），全开就留空，否则描述被噪声淹没；
3. 描述是**短清单**，完整自述留在 `GET /info` 的 `nodes[]` —— 一条信息只有一个出口，
   `execResult` 里**不**重复带 node 摘要。

#### 6.6.3 `/info` 的 `nodes[]`（人看的那份）

在现有 `{name, target, online, busy, default, nodeName, rootDir, shell, error}` 基础上补齐上面全部字段，
并增加 `protoVersion`、`platform`、`hostName`、`version`（构建号）、三个能力位、`unrestricted`、
`maxConcurrent`、`running`、`startTimeMs`、`diskFreeMB`。

管理台按 `online + running + lastError + unrestricted` 排序：**需要人看的三类**（离线 / 有 error / `unrestricted=true`）永远排前面。

#### 6.6.4 落地位置

| 改动 | 文件 |
| --- | --- |
| `helloAck` 字段扩到上表 | `binary/agent_cli/JZAgentNodePacket.{h,cpp}` |
| 填字段（构建号、平台、主机名、盘空间、运行态） | `binary/agent_cli/node/JZAgentNode.cpp`（`onLinkPacket` 的 hello 分支） |
| 存字段 + 暴露 getter | `binary/agent_cli/JZNodeLink.{h,cpp}` |
| `helloAck` 后刷新工具描述 | `JZAgentCli::startNodes` 的回调里调 `JZRemoteSshTool::refreshNodeList()` |
| 扩充 `/info` | `JZAgentCli::makeInfo` |
| 单测 | 扩 `test/test_remote_ssh_nodes.py`：断言 `/info` 里有 `platform`、`running=0`、`diskFreeMB>0`，且工具描述里出现"禁装包/禁联网" |

---

## 7. 配置（schema）

> 实际生效的字段以实现为准（`--check` 打印的就是它）：server 侧是 `http.*` + `hub.bind/port/token`，
> cli 侧是 `server.host/port/token/agentId`（**没有** `server.enabled`，`host` 非空即 managed）。

### 7.1 server `config.json`

```json
{
  "http": {
    "bind": "0.0.0.0", "port": 8088,
    "ssl": {
      "enabled": true, "requireClientCert": true,
      "certFile": "certs/server.crt", "keyFile": "certs/server.key", "caFile": "certs/ca.crt"
    }
  },
  "hub":       { "bind": "0.0.0.0", "port": 8090, "token": "<控制通道口令>" },
  "agents":    [ { "name": "local1", "cmd": "agent_cli", "args": ["-c", "a1.json"], "dir": "" } ],
  "selfAgent": { "enabled": true, "agentId": "server", "name": "server(内置)" }
}
```

| 字段 | 说明 |
| --- | --- |
| `http.ssl.enabled` | **默认 true**：管理台/API 走 HTTPS（`JZHttpServer::setSslConfig`）。置 false 才退回明文 HTTP，启动时打警告 |
| `http.ssl.requireClientCert` | **默认 true**：双向认证（服务端带 CA ⇒ mongoose `SSL_VERIFY_PEER\|FAIL_IF_NO_PEER_CERT`）。置 false = 单向 TLS |
| `http.ssl.certFile` / `keyFile` / `caFile` | PEM 路径，相对路径按**配置文件所在目录**解析（默认 `certs/`）。缺文件 ⇒ `init()` 直接失败，**不会**静默降级成明文 |
| `hub.bind` / `hub.port` | `JZNetServer` 监听（默认 8090），控制面专用，不能与 `http.port` 相同 |
| `hub.token` | 控制通道握手口令（全局单一口令） |
| `agents[]` | server **本机**托管的 cli 子进程（`name/cmd/args/dir/enabled`），与 hub 并存；**不是** hub 白名单，也没有 per-agent token |
| `selfAgent` | server 进程内置的 agent，见 §11 |

> server 进程**不再内嵌 frps**（2026-11-10 移除）；内网穿透需要时单独部署
> `jzfrps` / `jzfrpc`，与本架构的控制面互不依赖。旧配置文件里残留的 `frp` 段会被忽略。

证书自备（浏览器/客户端要装同一 CA 签的客户端证书；用自签 CA 免得被公用 CA 拒）：

```bash
mkdir -p certs
# 1) CA
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
  -keyout certs/ca.key -out certs/ca.crt -subj "/CN=agent-cli-ca"
# 2) 服务器证书(SAN 填实际访问的域名/IP; 否则浏览器按 IP 访问会报名字不符)
openssl req -newkey rsa:2048 -nodes -keyout certs/server.key -out certs/server.csr -subj "/CN=agent-cli-server"
openssl x509 -req -in certs/server.csr -CA certs/ca.crt -CAkey certs/ca.key -CAcreateserial \
  -days 3650 -out certs/server.crt -extfile <(printf "subjectAltName=DNS:localhost,IP:127.0.0.1")
# 3) 客户端证书(给管理员浏览器/脚本; 导入时连 ca.crt 一起装)
openssl req -newkey rsa:2048 -nodes -keyout certs/client.key -out certs/client.csr -subj "/CN=agent-cli-admin"
openssl x509 -req -in certs/client.csr -CA certs/ca.crt -CAkey certs/ca.key -CAcreateserial \
  -days 3650 -out certs/client.crt
openssl pkcs12 -export -in certs/client.crt -inkey certs/client.key -out certs/client.p12
```

### 7.2 cli `config.json`（managed）

```json
{
  "server": { "host": "10.0.0.5", "port": 8090, "token": "t-a1", "agentId": "a1" },
  "nodes": [
    { "name": "line1", "host": "127.0.0.1", "port": 7500, "token": "t-line1", "default": true }
  ],
  "llm": { "...": "不变" },
  "terminal": { "...": "不变（本地工作区/权限）" }
}
```

`server.host` 非空即 managed：**忽略** `http.*`（受控模式不再对外开 HTTP）。

`nodes[]` 每项对应一个 `link` 模式的 node（node 侧 `mode:"link"`、`host/port/token` 与这里一致）；
连接方式是 `host:port` + JZNet，不是 `/mcp`。只有一台时它就是默认；多台都没标 `default` 则按第一台。

### 7.3 cli `config.json`（standalone）

`http.*` 即可，`nodes[]` 取代 `mcp.servers[]`。standalone 也可以挂 node（本地执行 + 远端执行并存），
是否 managed 只影响控制面。

cli 配置分组（两种模式共用，managed 下忽略 `http.*`）：

| 分组 | 关键项 |
| --- | --- |
| `llm` | `url / name / token / netTimeoutMs / maxRounds / maxToken / maxTokens / maxRetries / maxFixRetries / retryDelayMs / body` |
| `terminal` | `rootDir / trustedDirs / shellProgram(仅 verify 脚本选解释器; 终端命令走内建 JZShell) / programs / recycleSec / allowTerminal / allowFileTools / allowPackageManager / allowNetwork / unrestricted` |
| `verify` | `maxRounds / timeoutMs` |
| `memory` | `enabled / file / maxBytes` |
| `system` | `extra / extraFile`（拼进 system prompt 一次） |
| `network` | `enabled / searchUrl / proxyHost / proxyPort / proxyUser / proxyPassword` |
| `mcp` | `enabled / servers[]`（仅第三方 MCP server，不用于 node） |
| `nodes` | `[{name, host, port, token, default}]`（link 模式 node，静态） |
| `task` | `queues / maxPending`（见 §9） |
| `http` | `bind / port / token / dir` |

| 顶层 | `agentName / version / logFile`（`logFile` 非空才有 `Agent.log`） |

### 7.4 兼容

- 旧 `mcp.servers[]` **保留可读**，但**只服务于第三方 MCP server**（不再是 node 的连接方式）。
  ⚠ 不做"旧 `http://node:7500/mcp` 自动转 `nodes[]`"：传输已经从 MCP 换成 JZNet，自动转会把 node 打成
  连不上的 `mcp` 模式——宁可启动时打一条 warning 说"node 请改用 `nodes[]` + node 侧 `mode:"link"`"，也不要静默错配。
- 旧 `agents[]{cmd,args,dir}`（server 本机托管）**保留**为一个独立模式 `type:"local"`，
  避免现有部署一升级就全废；但它与本文的 managed 是两套，文档里分开写。

---

## 8. 安全

| 面 | 机制 | 备注 |
| --- | --- | --- |
| 管理台 / API | 传输层：HTTPS + **强制客户端证书**（`http.ssl.*`，默认开） | 已取消应用层 `http.token`；能连上 TLS 的前提是拿到 CA 签发的客户端证书。`ssl.enabled=false` 时等于无鉴权，只能靠网络隔离 |
| 控制通道 | `hub.token`（握手派生密钥）+ agentId 身份 | token 不匹配表现是"包解不开"，靠 `hello` 超时给明确错误 |
| node（`link`） | `token`（JZNet 握手）+ 默认只绑内网 | 无 token 时**拒绝启动**；`mcp` 模式仍是"无鉴权"，必须靠网络隔离 |
| 模型侧 | node 的区域/白名单/危险命令策略照旧 | `unrestricted=true` 等于交出 shell，文档必须显著标注 |

**node 侧的三层防线**（与 `mcp`/`link` 无关，两条路共用）：

1. **区域**：`rootDir`（读写执行）+ 系统临时目录（读写）+ `trustedDirs`（默认只读），别处一律只读；
2. **命令策略**：程序白名单、`allowPackageManager`、`allowNetwork`；危险命令（dd/sudo/shutdown…）
   **无开关**，一律拒绝；
3. **网络隔离**：`mcp` 模式无内建鉴权，只能靠"绑 `127.0.0.1` / 内网 + 隧道"；`link` 模式有 token，
   但仍建议只绑内网。

⚠ 区域约束是**命令级**的（重定向目标写不了区外），程序自己做什么管不到（`rm -rf ~/x` 拦不住）。
真正的边界只能是沙箱。

**审计口径**：server 按任务记录 `{agentId, taskId, prompt, status, verify_rounds, 起止时间, 错误}`，落 `data/tasks.jsonl`。
⚠ 目前**没有"发起人"**：鉴权只在传输层（mTLS），也不记录客户端证书 CN，所以"谁交的"只能靠外部日志。

---

## 9. 任务队列（`task.queues`）

### 9.1 配置与语义

```json
"task": { "queues": 2, "maxPending": 32 }
```

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `task.queues` | `1` | 队列数，**每个队列一个独立 `JZAgent`（独立会话/上下文）**；1 = 旧行为 |
| `task.maxPending` | `32` | 全部队列都在跑时，最多排多少条；`<=0` = 不排队（忙就拒） |

- **队列内串行**：一个 `JZAgent` 只有一条会话，同一队列同一时刻只跑一个任务；
- **队列间并发**：不同队列互不影响，各跑各的；
- **派发**：提交时挑第一个空闲队列；全忙则进 master 的 FIFO 队列，任一队列空出来就派一条；
- **排满**：`maxPending` 到顶直接拒绝（`queue_full`，HTTP 侧回 429），不让队列变成任务黑洞。

### 9.2 实现方式（为什么不是"一个 JZAgentCli 里塞 N 个会话"）

每个队列是**一整个 `JZAgentCli` 实例**（`m_workers[i]`，`[0]` 就是 master 自己）：

| 每个队列独占 | 说明 |
| --- | --- |
| `JZAgent`（会话/上下文/工具表） | 工具对象把 `sigResult` 连到自己的 agent，**不能共用** |
| 一整套工具对象 + `JZVirtualTerminal` | 终端本来就按会话隔离；`endLoop()` 的 `cancelAll()` 只会清自己队列的后台命令 |
| 验证状态（`m_verifyProc/m_verifyCb/m_verifyGen/m_verifyOut`） | 两个任务同时验证会互相覆盖，必须每队列一份 |
| 重试/累积/回包状态 | 同上 |

这样**引擎代码（提交→验证→重试）一行没改**，只是被实例化 N 份；`submitTask` 变成调度器：

```cpp
submitTask(req, respond)          // 调度: 找空闲队列 -> dispatchTo(q) ; 全忙 -> m_pending
  └─ runTask(req, respond)        // 引擎本体(原 submitTask 的内容, 逐字未改)
```

master 负责且**只有 master** 负责：HTTP、控制面（`JZHubLink`）、node 连接、MCP 连接、SSE 广播、历史。

### 9.3 与其它模块的衔接

| 面 | 多队列下的行为 |
| --- | --- |
| 事件 | worker 通过 `m_eventSink` 交给 master；所有 SSE 帧（含 q0）都有 `queue` 下标，控制面按该队列的 `taskId` 上 `taskEvent` |
| `/info` | `queues: [{queue, busy, round, taskId, prompt}]` 与 `pendingTasks`；hub 的 `busy` 也按所有队列聚合，SSE 快照逐队列发送 |
| `/reset`（scope=task） | 排队任务逐条回一次 `cancelled`，再让每个队列各自收尾；取消期间禁止回包回调重入派发 |
| `taskCancel(taskId)` | 先查排队（命中就摘掉并回一次 `cancelled`），再按 `taskId` 只取消对应队列，不能误停其他队列 |
| node（`remote_ssh`） | worker **不自己连** node（借用 master 连接）；工具记录发起队列 owner，收尾只取消该 owner 的远端 exec |
| MCP（`mcp.servers[]`） | **只挂在队列 0**（见 §13） |
| 长期记忆 | 各队列写同一个 `memory` 文件（追加语义，交错但不丢内容） |

### 9.4 代价与风险（要认）

1. **并发写同一工作区**：两个队列可能同时改同一批文件，模型互相踩且不会报错 —— 队列数 >1 时应配独立工作区/子目录，或只让一个队列写本地；
2. **资源**：每个队列一套 agent + 工具 + 终端；`queues=4` 就是 4 倍内存与 4 条并发 LLM 连接（服务端限流要跟上）；
3. **工具面不齐**：MCP 只在 q0，q1+ 的模型没有那批工具（§13）；
4. **历史列表**：各队列记录自己的执行历史，master 在 `/info.recent` 按开始时间聚合最近 20 条并附 `queue`，不会在接口里重复出现同一任务。

---

## 10. 数据归属：server 存任务历史，cli 存执行状态

**server 是「任务历史」的主人，cli 是「任务执行」的主人。** 所以 server 必须落盘，cli 不需要（也不该）存历史。

| 数据 | 谁是权威 | 生命周期 | 放在哪 |
| --- | --- | --- | --- |
| 任务历史（谁在什么时候交了什么、结果如何） | **server** | 跨 cli 重启、跨 cli 更换、跨 server 重启都要在 | `<exeDir>/data/tasks.jsonl` |
| 任务执行（当前这一条的上下文/队列/验证进程） | **cli** | 任务结束即弃；进程重启即丢 | cli 内存 |
| agent 在线状态、节点、最近事件 | **cli 上报，server 缓存** | 心跳刷新，掉线即失效 | server 内存（`m_onlineByConn`） |

为什么不让 cli 存历史：

1. **cli 会重启**：升级、崩溃、客户手动重启都会丢；
2. **cli 可能被替换**：同一 agentId 换机器上线，历史应该延续；
3. **多队列 + 多 cli**：历史分散在 N 个进程里，前端要合并、要处理时钟差异；
4. **验收/审计在提交方一侧**：谁有资格看历史，和"谁执行"是两件事 —— server 天然是收口点。

反过来，**cli 也不该存全量历史**：它没有持久化层，硬加一个只会和 server 的记录打架。

### 10.1 不一致怎么处理

| 场景 | 现象 | 规则 |
| --- | --- | --- |
| **cli 重启** | 它不认识之前的任务；正在跑的丢了 | server 记录照旧；非终态 Task 在心跳/下线对不上时标 `TASK_STATE_UNSPECIFIED` |
| **server 重启** | 内存里的在跑任务全丢 | 载入 `tasks.jsonl` 时把非终态一律改 `UNSPECIFIED`，`TaskStatus.message="server 重启, 结果未知"` |
| **cli 掉线 / hello 断开** | 不可能再收到 `taskEnd` | 该 agent 名下非终态 Task 标 `UNSPECIFIED`；提交方（如果在等）回 502 + Task |
| **超时** | server 侧 `taskTimeoutMs` 到点 | 标 `TASK_STATE_FAILED` + 原因，同时下发 `taskCancel` |

**判定口径**：Task 处于 `SUBMITTED/WORKING/INPUT_REQUIRED/AUTH_REQUIRED` 时以 server 收到的 `taskEnd` 为准；
`COMPLETED/FAILED/CANCELED/REJECTED/UNSPECIFIED` 一旦形成，迟到的 `taskEnd` 不得覆盖。
没配 `verifyCmd` 时 cli 会把 agent 报错也判成 success（映射成 `COMPLETED`），此时 `TaskStatus.message` 仍带错误文本。

### 10.2 存储形态

- **JSONL 追加**：每行就是一个完整的 A2A `Task`（`id/contextId/status/artifacts/history/metadata`）；载入时按 `id` 后写覆盖先写。**旧格式行直接跳过**，不做兼容解析。
- 内存里保留最近 **500** 条（超出按插入顺序淘汰），启动时全量载入。
- 过程事件放 `metadata.events`（有界 200 条），每 20 条 + 终态各落盘一次。
- ⚠ 文件本身**不轮转**（见 §13）。

### 10.3 接口（只有 A2A 一套）

| 路径 | 作用 |
| --- | --- |
| `POST /api/tasks` | `SendMessage`：`{agentId, message:{role,parts}, contextId?, verifyCmd?, returnImmediately?, taskTimeoutMs?, ...}`。默认阻塞到终态；`returnImmediately:true` 立即回 `202` + 一个 A2A `Task`。cli 的 `/task` 载荷只在 server 边界转换一次 |
| `GET /api/tasks` | `ListTasks`：`agentId` / `state` / `contextId` / `limit` → `{tasks[], nextPageToken, pageSize, totalSize}` |
| `GET /api/tasks/detail?id=` | `GetTask`：一个 A2A `Task`；管理信息在 `metadata` |
| `GET /api/tasks/subscribe?id=` | `SubscribeToTask`（SSE）：首帧当前 Task，随后 `status_update`/`artifact_update`，终态补完整 Task 后关流，15s 保活 |
| `GET /api/agent/card?id=` | `AgentCard` |
| `POST /api/tasks/cancel?id=` | `CancelTask`：幂等、不保证成功，返回更新后的 Task（状态不立即改） |

> 网页一律 `returnImmediately:true`；Task 详情只走 SSE，断线由 `EventSource` 自动重连、首帧补当前状态，不再维护轮询兜底。
> 多轮追问按 §3.4.3 的「同 `contextId` 新建 Task」实现，不改 hub 协议；结果放 `Artifact`，`Message` 只进 `history`。

---

## 11. 管理台与 server 内置 agent

### 11.1 页面结构：连接列表 → 详情

管理台按 **A2A v1.0「点名一台 agent 下发指令 → 跟踪一个 Task」** 组织（2026-11-10 改造）：
参照系是 agent 平台/协议（A2A 的 `SendMessage` / `Task` / `AgentCard`），不是 CI runner。

| 路径 | 页面 | 内容 |
| --- | --- | --- |
| `/` | `index.html` | **Agent 台账**（AgentCard 视图：名称/版本/模型/工作区/能力/忙闲）+「进行中 Task」区（可直达详情、可取消）。卡片上直接「下发指令」 |
| `/new` | `new.html` | `SendMessage` 表单：点名 agent + 指令 + 可选验证命令 + 可选 `contextId`；提交后跳 `tasks?id=` |
| `/tasks` | `tasks.html` | **Task 中心**（流程终点）：`ListTasks` 列表 + `GetTask`/`SubscribeToTask` 详情 = 状态头、事件时间线、Artifacts 产物区、History、取消、同 `contextId` 追问 |
| `/agent?id=` | `agent.html` | 某台 agent 的台账：指令流 / Task 列表 / AgentCard（skills、capabilities、节点），输入框带 `contextId` 延续 |

一个 Task 一个稳定 URL（`tasks?id=`）；派活只有一份 `sendMessage()` 实现。会话按 `contextId`
分组，追问按规范 3.4.3 生成同上下文的新 Task，而不是往运行中的 Task 里塞消息（不用改 hub 协议）。

`GET /api/agent?id=` → `{ kind, info, tasks[] }`，`kind ∈ {hub, builtin, process, offline}`。

### 11.2 为什么 server 自己也要有一个 agent

管理动作（看集群、给某台派任务、查记录、启停本机子进程）**只有 server 知道**，远端 cli 的工具表里既没有也不该有（那是控制面权限，属主不是执行面）。

所以 server 进程内直接跑一个 `JZAgent`（`JZServerAgent`）：

- 出现在 `/api/status` 的 `online[]` 里，`agentId` 默认 `server`，带 `builtin=true`；
- 与远端 cli **走同一套接口**：`POST /api/tasks {agentId:"server"}`、`GET /api/agent?id=server`、任务记录同样落 `tasks.jsonl`；
- 区别只在执行位置：任务在 server 进程内跑，不下发 hub。

配置：

```json
"selfAgent": {
  "enabled": true, "agentId": "server", "name": "server(内置)",
  "rootDir": "...", "modelUrl": "...", "modelName": "...", "modelToken": "...",
  "maxRounds": 40, "maxToken": 100000
}
```

### 11.3 server 独有工具

| 工具 | 作用 |
| --- | --- |
| `server_agents` | 集群视图：在线 cli（忙闲/节点/工作区）+ 本机子进程 + 任务计数 |
| `server_task_submit` | 把指令发给任意 agentId（**强制 `returnImmediately:true`**，工具里同步等会把自己卡死） |
| `server_task_query` | 查任务记录（列表/单条，可按 agent/status 过滤） |
| `server_agent_control` | 启停/重启 server 本机被监管的 `agents[]` 子进程 |

本地执行面（`terminal_run/query/cancel`、`read_file`、`get_datetime`）与 cli 用的是同一份 `agent_tool` 实现，
只是终端实例独立（`id="server"`）。`selfAgent.rootDir` 通过独立 `ICodeToolHost` 同时喂给终端区域配置与 `read_file`，
不是只写进提示词；取消时显式 `cancelAll()` 停本地进程，终态为 `cancelled`。

### 11.4 内置 agent 的边界

- **不参与 verify 门控**：server 本机没有"产物"概念，任务结束即结束，`verify_rounds` 恒为 0；
- 只能"通过任务"影响远端 cli，读不到远端文件 —— 这是权限边界，不是缺失；
- 忙时 `POST /api/tasks` 回 409（单实例，没有队列；需要队列就用远端 cli 的 `task.queues`）。

### 11.5 列表 = 「曾经连过的全部」（含离线）

| 项 | 规则 |
| --- | --- |
| 档案 | `data/agents.json`（整文件重写）：`agentId/name/version/rootDir/modelName/firstSeen/lastSeen/connects` |
| 接口 | `GET /api/status` 的 `known[]`：**在线优先**，其次按 `lastSeen` 倒序；`online` 由 `m_connByAgent` 现算 |
| 离线详情 | `GET /api/agent?id=` 依次查 在线 → 档案（`kind=offline`）→ 本机子进程；离线时详情页**禁用派活表单** |
| 删除 | `POST /api/agent/forget?agentId=`：在线 409、内置 agent 400；默认只删档案，`withTasks=1` 连任务记录一起删（并重写 `tasks.jsonl`，否则追加文件会在重启时"复活"已删行） |
| 心跳回收 | hub 每 5s 扫一次，`now - lastSeen > 20s` 判离线 |

**刷新口径按"数据还会不会变"分**：会变的（单个进行中 Task）只走 `GET /api/tasks/subscribe?id=` 的 SSE：
首帧补当前快照，事件逐帧推，终态自动关流；断线由 `EventSource` 自动重连并补首帧，**不再维护第二套轮询**。
不会变的（Agent 台账、历史列表）只「手动刷新 + 改筛选拉一次」。首页「进行中」是聚合视图、没有对应 SSE 端点，
用 1.5s 条件跟随且只重绘该区块。页面隐藏时暂停跟随，不整页重绘（否则正在读的详情会被刷掉，表现为一直闪）。

### 11.6 静态资源的缓存策略

`JZHttpStaticFiles` 默认 `max-age=3600`，改完页面而浏览器还跑着缓存里的旧 `app.js`，
表现就是"HTML 是新的、脚本是旧的" → 会话区一直停在「加载中…」，从界面上完全看不出原因。现在：

- 管理台对静态文件设 `setCacheMaxAge(0)`（`max-age=0` + ETag/Last-Modified，没变就 304）；
- 页面里引用资源带版本号 `/app.css?v=5`、`/app.js?v=5` —— **改前端资源时要顺手 +1**；
- `app.js` 里加了 4s 兜底提示：仍停在「加载中」就直说"可能是缓存，Ctrl+F5"。

---

## 12. 路线图

| 阶段 | 内容 | 验收 |
| --- | --- | --- |
| **P3** | `logAppend` 落盘、离线策略、审计补齐 | 断电/重连/离线各场景行为可预期；managed 现场能看日志 |
| **P4**（可选） | node 自述（§6.6）：身份/能力位/运行态 + 刷新 `remote_ssh` 描述 | `/info` 里有 `platform`、`diskFreeMB`；工具描述里出现"禁装包/禁联网" |

依赖关系：P3 与 P4 互不依赖。

---

## 13. 已知缺口与未决

**功能缺口**

- **没有 `logAppend`**：server 侧看不到 cli 的控制台日志；managed 模式下 cli 也没 HTTP，现场排障只能看 cli 自己落盘的 `Agent.log`。这是 managed 模式最该先补的一项。
- **没有 agent 白名单 / per-agent token**：任何拿到 `hub.token` 的 cli 都能连上（全局单一口令）。
- ~~管理台没有事件流~~（2026-11-10 已补）：server 在 `Task.metadata.events` 保留有界事件序列（上限 200 条，每 20 条 + 终态落盘），并提供 `GET /api/tasks/subscribe?id=` 的 SSE 订阅。**仍未做**的是 cli 控制台日志的 `logAppend`，以及管理台按轮次展示完整工具输出/trace。
- **MCP 只在队列 0**：`mcp.servers[]` 只挂 q0（每队列一套会让 stdio MCP 被拉起 N 次），q1+ 的模型工具面不齐。
- **审计没有发起人**：只有 mTLS，不记客户端证书 CN，`tasks.jsonl` 里也没有 requester 字段（§8）。
- **`tasks.jsonl` 不轮转**：内存 500 条，文件无限追加且启动全量载入，长期运行会变慢并占盘。

**未决**

- **`hub.token` 为空仍可启动**：server 侧只打 `token=(无)` 就监听 `hub.bind`（默认 `0.0.0.0`），
  等价把 `taskSubmit` 交给任何能连上 8090 的人。node 的 `link` 模式对空 token 是硬拒绝启动，两侧标准不一致。
  倾向：空 token 拒绝启动（或强制绑回环）。
- **大包策略**：下行 `taskSubmit`（`images` 的 dataURL）与上行大 `taskEnd` 的传输/截断语义目前靠 §4.4 的截断兜住一半，
  下行没兜。要不要限制图片大小、要不要给下行也加裁剪，待单独定。
- **managed 模式是否保留本地诊断端口**（只绑 `127.0.0.1` 的 `/info`、`/events`）？
  完全不起的话，现场排障只能看 server 侧；起了又不满足"不对外起 HTTP"的字面要求（需明确"仅环回不算对外"）。
- **§1.2 的"假通过"要不要机器化治理**：目前只有"写验证命令要小心"的口头约束。
  可选做法：任务级唯一 workdir（`<rootDir>/runs/<taskId>`）后跑验证，或验证前校验产物 mtime/哈希。
- **一个 node 被多台 cli 共享**的行为未定义：node 侧 `maxConcurrent` 是每连接还是全局？多 cli 同时打会串行阻塞，
  审计里也只能靠 `clientId` 区分。

---

## 14. 实现记录

### 14.1 文件

| 文件 | 作用 |
| --- | --- |
| `binary/agent_cli/JZAgentNodePacket.{h,cpp}` | 执行面协议：`NodePack_hello / helloAck / exec / execResult / execCancel` + `JZAgentNodePacketInit()`（幂等注册，必须在 `JZNetPackInit` 之后） |
| `binary/agent_cli/JZAgentHubPacket.{h,cpp}` | 控制面协议：`HubPack_*` + `JZAgentHubPacketInit()`（包号 `NetPack_user+101…`，与 node 的 `1000…` 不冲突） |
| `binary/agent_cli/node/JZAgentNode.{h,cpp}` | `initLink()`（只建一台终端，**不建 JZMcpServer**）+ `startLinkServer()` + `onLinkPacket()` + `runLinkExec()` |
| `binary/agent_cli/node/main.cpp` | 按 `config.mode` 分支：`link` → `initLink/startLinkServer`；否则原 MCP 路径 |
| `binary/agent_cli/JZNodeLink.{h,cpp}` | `JZNodeLink`（`JZNetClient` 连接 + 重连 + 单条在飞 + 超时收口）+ `JZRemoteSshTool` |
| `binary/agent_cli/JZHubLink.{h,cpp}` | `JZHubLink`（`JZNetClient` 连接 + hello/心跳 + 重连退避 + 事件/回包上行 + 任务下行回调 + 包大小裁剪） |
| `binary/agent_cli/JZAgentCli.{h,cpp}` | 配置结构 / `managed()` / `startNodes()` / `startServerLink()` / 多队列调度 / `nodesJson()` / `makeInfo` |
| `binary/agent_cli/main.cpp` | 解析 `server.*` 与 `nodes[]`；`--check` 打印运行模式；standalone/managed 启动分支 |
| `binary/agent_cli/server/JZAgentCliServer.{h,cpp}` | `startHub()`（`JZNetServer`）+ 在线注册表 + 任务记录/store + `/api/*` + 网页 |
| `binary/agent_cli/server/JZServerAgent.{h,cpp}` | server 内置 agent（§11） |
| `binary/agent_cli/server/main.cpp` | 解析 `http.*` / `hub.*` / `agents[]` / `selfAgent`，默认配置模板，`--check` |
| `binary/agent_cli/server/web/*` | 管理台（A2A 形状：`index/agent/tasks/new` + `app.js` + `app.css`），样式走 `web/ui` 的 jz-ui |
| `binary/agent_cli/test/fake_llm.py` | 固定返回「任务已成功」的假 LLM（OpenAI 兼容 SSE），离线跑全链路用 |
| `binary/agent_cli/test/test_a2a_flow.py` | 一键自检：假 LLM → server → cli(managed) → 网页/API/SSE（`ctest -R agent_cli.a2a_flow`；`--serve` 可手动开网页） |

### 14.2 行为要点（node / remote_ssh）

- **并发**：每个 node 同时只跑 1 条 exec（`maxConcurrent` 默认 1），忙时 `remote_ssh` 直接回 "节点 X 正忙"；**不排队**（排队会变成任务黑洞）。后台命令也占这个额度。
- **runId**：`<节点名>:r<序号>`（如 `line1:r3`），`query`/`cancel` 靠前缀路由回节点；节点侧维护
  协议 runId → 终端 runId 的映射（上限 1024，超了先淘汰已结束的）。
- **后台**：`background=true` 时节点调 `runBackground` 并立刻回 `started`；完成时**不再回包**
  （不挂连接），状态/日志用 `action=query` 取。cli 侧把这些 runId 记进 `m_bgRuns`，`busy()` 计入、
  任务收尾时按 owner 发 `execCancel`。
- **查询**：同一时刻只允许一条查询在飞（10s 超时）。`tail`/`grep`/`incremental` 在节点侧过滤，
  游标 `advanceRunReadPos` 也在节点侧推进，轮询不会重复灌同一段日志。三条与本地一致的硬规则：
  省略 `run_id` = 该节点最近一次运行（回包里补上实际 id）；**带 `grep` 时不推进游标**
  （否则被过滤掉的行下一轮就永远读不到）；回包按 **32KB** 截断并提示"还有 N 行未显示"
  （运行记录窗口本身可到 4000 行 / 512KB，不裁会顶爆 256KB 单包）。
- **输出裁剪**：`execResult.output` 超 **120KB** 时按"头 60KB + 一行显式提示 + 尾 60KB"裁剪（UTF-8 字符边界），远低于 256KB 包上限。
- **超时双保险**：cli 侧 timer 到点 → 发 `execCancel` + 本地收口；node 侧 `run(timeoutMs)` 到点杀进程树。上限 15min。
  后台没有执行超时，但"节点确认已启动"有 15s 超时（没确认就是结果未知，不重放）。
- **token 必填**：`link` 模式下 `token` 为空直接拒绝启动（否则等于把 shell 交给任何能连上端口的人）。
- **cwd**：非空时拼成 `cd "<cwd>" && <command>` —— 终端的解析器本来就支持 `cd X && cmd`，并按 X 重新审权限，所以没有为它新开 API；含双引号的 cwd 直接拒绝。
- **`/info`**：`nodes[]` 每项给出 `{name, target, online, busy, default, nodeName, rootDir, shell, error}`，
  控制台日志是缓冲的时候靠它自检（token 错的表现是 `online=false` + error 文案）。
- **重连**：`sigDisconnected` → 1/2/4/…/30s 退避；断线时在飞的 exec 以"结果未知"收口，不重放（远端可能已经执行过）。
- **控制面**：`agentId` 缺省取 `agentName`；同一 `agentId` 重连时旧连接被 `closeClient` 顶掉（管理台不会留两行）。
- **server 侧**：`POST /api/tasks` body 与 `POST /task` 完全一致（`task_timeout_ms` 只给 server 用，不下发）；
  离线 agent 回 404；请求不排队到离线机器。

### 14.3 踩过的坑（以后再加类似功能别再踩）

| 坑 | 现象 / 根因 | 做法 |
| --- | --- | --- |
| mock 模型必须回 SSE | agent_cli 是流式（`setStreamEnabled(true)`），回普通 JSON 会在 `sigReadyRead` 里被当 SSE 消费掉，到 `onLLMRequest` 只剩空 body（报 "json解析错误 ... body=[]"） | mock 回 `data: {...}\n\n` |
| Python mock 的读法 | 请求体带 `Content-Length`、`Connection: close`（不是 chunked） | `http.server` 按 `Content-Length` 读 |
| 静态资源缓存 | 见 §11.6 | `cacheMaxAge(0)` + `?v=N` |
| `JZNetServer` 断开不通知 | `onClientDisconnect` 用 `socket->m_conn` 反查，而 `closeInternal()` 已先把它复位成 `INVALID_NET_HANDLE`，`find(0)` 必然失败 → 早退 → `sigDisconnected` 不发 | 改为按 **socket 指针**反查 `m_clients`（连接数很小，O(n) 足够） |
| `closeClient()` 崩进程 | 找不到连接时先 `JZ_ASSERT`（只记日志、不中断），随后仍执行 `it->second`（`end()`）→ UB | 找不到直接 `return`（**关闭是幂等的**） |

修完之后：杀 cli 后 4s 内列表就变"离线"，且 `closeClient` 可以安全地用于清理死连接。

### 14.4 测试

```powershell
ctest --test-dir build -C Debug -R "agent_cli\.(remote_ssh_nodes|hub_nodes|task_queues|task_cancel|server_tasks)$" --output-on-failure
```

- `agent_cli.remote_ssh_nodes`：起 2 台 link node + 1 个 standalone cli，把 nodeA 的 secret 搬到 nodeB；
  断言两台 node `online=true` → 任务 `success` → nodeB 文件内容与随机 secret **逐字符相等** →
  验证命令只读 nodeB 的文件（cli 本地工作区没有它），所以必须真的经 `remote_ssh` 搬运。
  同一用例还覆盖 `background`/`query`/`cancel`：同步写一行 → 后台上 `sleep 3` 再追写第二行 →
  `query`（应看到"运行中"）→ `cancel`；**等过 3s 再读文件**，第二行不能出现（证明取消真杀了进程）。
  mock 模型会检查每轮"上一条 tool 结果"是否含预期标记，落空即判失败（不依赖被缓冲的节点日志）。
- `agent_cli.hub_nodes`：2 台 link node + 1 个 managed cli + 1 个 server；任务从 server 的 `POST /api/tasks` 提交；
  断言 `online[]` 有该 cli 且两台 node 在线 → `success` → 有 `lastEventType` → cli 的 `http.port` **未被监听**（模式互斥）。
- `agent_cli.task_queues`：mock 每个请求睡 1.5s，`queues=2`；A/B 同时提交 → `/info.queues` 两条 busy；
  全忙提交 C → `pendingTasks=1` 且最终 success；总耗时 3.1s（串行应 4.5s）。
- `agent_cli.task_cancel`：queues=1/2 的"在跑 + 排队"reset、worker SSE、recent 聚合、真实 node 进程取消。
- `agent_cli.server_tasks`：managed 排队/在跑任务的 `returnImmediately:true` 取消、多队列 busy、异步超时、迟到 `taskEnd`、chunked SSE reply 上行。

以上全部用内置 mock 模型，不联网，秒级。
