# Agent 控制面设计：server → cli → node

> 2026-11-06 成文。**状态：P0（node `link` + cli `nodes[]` + `remote_ssh`）、P1（模式互斥）、P2（控制通道）已落地；
> P3 与 §6.6（node 自述）未实现**。
>
> **本文是这份架构唯一的文档**：目标与职责划分、协议、配置 schema、取舍与路线图，外加 P0 的实现记录（§12）。
> 部署/排障的细节以各程序的 `--init` 生成物和 `--check` 输出为准（它们永远与代码同步），本文只保留设计与目标。
>
> P1（cli 模式二选一）与 P2（JZNet 控制通道）仍是设计，与当前代码的差异逐条列在 §10。

---

## 1. 目标与硬约束

一句话：**server 是唯一对外入口与管理面，cli 是执行代理（也能独立跑），node 是可选远端执行代理，只以一个 `remote_ssh` 工具出现在模型面前。**

六条硬约束（来自需求，实现时不得偏离）：

| # | 约束 | 含义 |
| --- | --- | --- |
| 1 | cli 支持两种模式 | `standalone`（自带 HTTP，可选 frp）/ `managed`（受 server 控制） |
| 2 | 两模式**互斥** | `managed` 时**不起 HTTP、不起 frp**；反过来 `standalone` 时不上报 server |
| 3 | server↔cli 走 **JZNetClient** | 用框架现成的 `JZNetClient`/`JZNetServer` + 自定义包，不自造协议 |
| 4 | node **预配置、不动态** | 启动时按配置连好；运行期不增删节点；cli 侧暴露为**单个工具**（remote_ssh 语义） |
| 5 | **执行面默认本地** | cli 不挂 node 时自己执行（`terminal_*` + 文件工具），node 是可选旁路 |
| 6 | **node 有两种模式，但角色固定** | `mcp`（起 `JZMcpServer`，行为同现状）/ `link`（起 `JZNetServer`，**不起 MCP**）；两种模式 node 都是 **server**，cli 永远是 **client** |

### 1.1 node 保留，但它是可选层（2026-11-06 定）

**主形态（默认）**：`server → cli ×N`，每台 cli 在**自己那台机器**上本地执行。
分布式执行由"多台 cli"实现，不需要 node。

**已决定保留 node**：因为现实里存在"**目标机器不能访问网络 / 装不了 cli**"的场景
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

### 1.2 一条硬耦合：验证面必须跟执行面走

`runVerifyAsync` 现在用 `JZProcess` 在 **cli 本机** 起进程，`cwd = m_config.rootDir`（`JZAgentCli.cpp:1731`、`:1749`）。
所以一旦"任务在 node 上执行、验证在 cli 本地跑"，**必然假失败**（本机没那些文件）。

⇒ 规则：`/task` 增加 `verify_host`（默认 = 本次任务的执行 host；`local` 表示本机）。
任务在 node 上执行时，验证命令也下发到同一 node 执行；否则不做远端执行。

**明确不做**：

- ⛔ 不做运行期动态加/删 node（不搞"注册中心发现节点"）；
- ⛔ 不做 server 侧的 agent 自动扩缩容 / 任务队列持久化（第一版只做"在线就派、离线就拒"）；
- ⛔ 不在 node 上发明第二套协议（node 仍是执行面，不是控制面参与者）；
- ⛔ 不在 cli 里加"多轮调度"（跑几轮/何时停归提交方，是既有约定）。

---

## 2. 三个平面

| 平面 | 协议 | 承载 | 谁发起 |
| --- | --- | --- | --- |
| **控制面** | JZNet（TCP + 握手 token + ChaCha20） | 注册 / 心跳 / 任务下发 / 取消 / 事件回传 / 日志回传 | **cli → server（出站，穿透 NAT）** |
| **任务面** | 同上（packet）或 cli 本地 HTTP（仅 standalone） | 提交一次任务、拿一次回包 | 提交方 / server |
| **执行面（本地，默认）** | 进程内 `JZVirtualTerminal` + 文件工具 | `terminal_*` / `read_file`… | cli 自己 |
| **执行面（远端，可选）** | **JZNet（`NodePack_exec`）**：node `link` 模式起 `JZNetServer`，cli 用 `JZNetClient` 连 | `remote_ssh` 一条命令 | **cli → node** |

关键点：**控制面由 cli 主动连 server，执行面也由 cli 主动连 node** —— 整条链路上 cli 是唯一的 client，
server 与 node 都是 server 角色。这样内网机器不需要 frp、不需要端口映射，两端也不必知道 cli 的地址。

---

## 3. 两种运行模式

```
(A) standalone（现状语义，保留）
   提交方/浏览器 ──HTTP──► cli :8080 ──► JZAgent ──► 本地工具 / remote_ssh(node)
                                   └──(可选)frpc──► 外部 frps

(B) managed（新增）
   server :8088（管理台/API）
        ▲  JZNetClient 出站连接（token）
        │
      cli（无 HTTP、无 frp）──► JZAgent ──► 本地工具 / remote_ssh(node)
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
    cli.startServerLink();   // JZNetClient 连 server；不建 JZHttpServer、不建 JZFrpClient
else
{
    cli.startHttp();         // POST /task + 网页控制台
    cli.startFrpc();         // 可选
}
return app.exec();
```

### 3.3 互斥规则表

| 能力 | standalone | managed |
| --- | --- | --- |
| `JZHttpServer`（`/task` `/info` `/events` `/reset` `/terminal*`） | 起 | **不起** |
| 网页控制台 `http/` 静态资源 | 起 | **不起** |
| `JZFrpClient`（frpc） | 按配置 | **强制关**（配了也忽略并打 warning） |
| `JZNetClient`（连 server） | 不连 | 必连 |
| 任务来源 | 外部 `POST /task` | server 的 `task_submit` |
| 取消/重置 | `POST /reset` | `task_cancel` / `reset` 包 |
| 诊断 | 网页 + curl | **只能靠 server 侧视图**（见 §11 未决问题 3） |

⚠ 这条最容易漏：`POST /reset` 是长程装置/评测脚本杀任务的正规手段（`agent_cli.py` 超时就靠它）。
managed 模式下必须提供等价的 `task_cancel` 包，否则任务没法中断。

---

## 4. 控制通道协议

### 4.1 传输

复用 `JZNetClient` / `JZNetServer`（`JZNet/JZNetClient.h`、`JZNet/JZNetServer.h`）：

- 客户端：`setToken()` → `connectToHost(ip, port)` → `sigConnected` → `sendPacket()` / `sigPacketReceive`；
- 服务端：`setToken()` → `listen(port)` → `sigAccept` + `nextPendingConnection()` → `sigPacketReceive(connId, pack)` / `sendPacket(connId, pack)`；
- 握手由框架内部完成：`JZNetPackHand(nonce)` → `JZNetPackHandRes`，双方用 `"JZ" + nonce + token` 派生 ChaCha20 密钥，
  **之后的包都是加密的**（见 `JZNetClient.cpp:194-278`）。

⚠ **token 的校验语义要清楚**：握手只校验"客户端带没带 nonce"与服务端"有没有 token"是否一致
（`hand->nonce.empty() != m_token.empty()`），**不比对 token 内容**。token 写错的表现不是明确报错，
而是密钥不同 → 后续包解不开 → 连接被关。所以：

- 协议里必须有应用层 `hello` / `hello_ack`，**3 秒内没收到 `hello_ack` 就判定 token 错误**并给出明确日志；
- 错误信息别只说"连接断开"，要提示"token 不匹配"这个最可能的原因。

### 4.2 包类型

从 `NetPack_user`(1000) 起，集中在一个头文件（仿 `JZFrpPacket.h` + `JZFrpPacketInit()`）：

```cpp
enum {
    HubPack_hello = NetPack_user + 1,   // cli -> server
    HubPack_helloAck,                   // server -> cli
    HubPack_heartbeat,                  // cli -> server
    HubPack_taskSubmit,                 // server -> cli
    HubPack_taskCancel,                 // server -> cli
    HubPack_reset,                      // server -> cli
    HubPack_taskEvent,                  // cli -> server
    HubPack_taskEnd,                    // cli -> server
    HubPack_logAppend,                  // cli -> server
};
void JZAgentHubPacketInit();   // 两边启动时各调一次（在 JZNetPackInit 之后）
```

⚠ 注册时机：必须在 `JZNetPackInit()`（`JZApplication` 构造时已调用）**之后**
`JZNetPackManager::instance()->registPack<T>(type)`，否则收到的是 "unknown packet"。

### 4.3 消息表

| 方向 | 包 | 字段 | 说明 |
| --- | --- | --- | --- |
| cli→server | `hello` | `agentId, name, version, rootDir, modelName, mode, nodes[]` | 上线自报。`agentId` 必须与 server 配置里登记的一致 |
| server→cli | `helloAck` | `serverTime, acceptTasks` | `acceptTasks=false` 时 cli 只上报不接任务（维护模式） |
| cli→server | `heartbeat` | `busy, round, taskId, uptimeMs` | 5s 一次；15s 无心跳判离线 |
| server→cli | `taskSubmit` | `taskId` + `/task` 的全部字段（`prompt/images/verify_script/verify_cmd/verify_timeout/max_verify_rounds/max_retries/max_fix_retries/retry_delay_ms/use_memory`） | 语义与现有 `POST /task` **完全一致**，cli 直接复用 `submitTask()` |
| server→cli | `taskCancel` | `taskId` | 等价 `POST /reset {"scope":"task"}` |
| server→cli | `reset` | `scope`（`task` / `history`） | 等价现有 `/reset` |
| cli→server | `taskEvent` | `taskId, type, data` | **`type` 直接复用现有 SSE 的取值**：`thinking/reply/tool_call/tool_result/verify/error/terminal_cleanup` |
| cli→server | `taskEnd` | `taskId` + 现有回包字段（`status/verify_rounds/retries/retries_transient/retries_fix/agent_reply/last_verify_output/error`） | 一次任务一次，务必送达（见 §5.3） |
| cli→server | `logAppend` | `lines[]` | 控制台日志的增量（server 侧按 agentId 落盘，替代 v1 读子进程 stdout） |

**不做第二套事件语义**：模型在干什么（think / tool_call / verify）这套词已经在 SSE 里定死了，
packet 里换名字只会让两套 UI 对不上。

### 4.4 大小与分片

- 单包上限 **256KB**（`MG_MAX_PACK_SIZE`，`JZNetPacket.h:11`）；
- `tool_result` / `last_verify_output` / 长日志**很容易超**，这是本协议最容易踩的坑；
- 规则：
  - 单条消息序列化后 > 128KB ⇒ 拆成 `chunk {msgId, index, total, payload}`；
  - 文本类内容先按"头 4KB + 尾 4KB + `...[截断 N 字节]...`"压缩再决定要不要分片
    （UI 和模型都不需要中间那段几 MB 的编译日志）；
  - 分片在接收侧按 `msgId` 重组，超时（10s）未收齐则丢弃并记一条 warning。

### 4.5 心跳、重连、离线

| 参数 | 值 | 说明 |
| --- | --- | --- |
| 心跳间隔 | 5s | cli 主动发 `heartbeat` |
| 判离线 | 15s | server 侧超过 3 个周期没收到即置 `offline`，管理台灰色 |
| 重连退避 | 1/2/4/8/…/30s 封顶 | cli 侧 `sigDisconnected` 后按序重试，成功后清零 |
| 重连补发 | `hello` + 最新状态快照（`busy/round/taskId`） | 让 server 重建视图 |

---

## 5. 任务生命周期

### 5.1 正常路径

```
提交方 ──POST /api/tasks{agentId, ...}──► server
                                          │ 选 agentId（离线/忙 → 直接 4xx 拒绝）
                                          ▼
                                    taskSubmit ──► cli
                                                   │ submitTask()（现有实现，不动）
                                                   │ ├─ taskEvent(thinking/reply/tool_call/tool_result/verify)…
                                                   ▼
                                              taskEnd ──► server（归档 + 推管理台 SSE）
                                                             │
提交方 ◄────────────── 200（同步等待式）／或先返 202 + 订阅 SSE ───────────────┘
```

- **任务在 cli 里跑**：LLM 调用、验证循环、重试预算、长期记忆全不动；server 只做转发与聚合。
- server 的 `/api/tasks` 两种回包方式：同步等 `taskEnd`（保持现有 `/task` 手感）或先回 `202 {taskId}` 再走 SSE；
  第一版建议**同步**，与现有提交方（`agent_loop.py`、benchmark）语义一致。

`taskSubmit` 就是现有 `POST /task` 的请求体，`taskEnd` 就是它的回包（语义完全一致，便于 P2 直接复用）：

| 请求字段 | 说明 |
| --- | --- |
| `prompt` | 任务文本（与 `images` 至少一个非空） |
| `images` | dataURL / http 链接数组，或 OpenAI `messages` 里的 `image_url` |
| `verify_script` / `verify_cmd` | 验证脚本文件 / 直接命令（`verify_cmd` 优先） |
| `verify_timeout` / `max_verify_rounds` | 覆盖配置里的 `verify.timeoutMs` / `verify.maxRounds` |
| `max_retries` / `max_fix_retries` / `retry_delay_ms` | 覆盖 `llm.*` 的重试预算 |
| `use_memory` | `false` = 本次不注入长期记忆 |

| 回包字段 | 说明 |
| --- | --- |
| `status` | `success` / `failed` / `cancelled` |
| `verify_rounds` / `retries`（`retries_transient` / `retries_fix`） | 轮次与重试统计 |
| `agent_reply` / `last_verify_output` / `error` | 结论、最后一次验证输出、失败原因 |

### 5.2 取消与重置

| 场景 | 消息 | cli 行为 |
| --- | --- | --- |
| 提交方主动取消 | `taskCancel{taskId}` | 等价 `/reset scope=task`：中止在途 LLM/验证，回 `taskEnd{status:"cancelled"}` |
| 清会话（换新任务前） | `reset{scope:"history"}` | 只清对话历史，不动在途任务 |
| server 判定卡死 | `taskCancel` + 记审计 | 同第一行 |

### 5.3 断线与崩溃

| 情况 | 处理 |
| --- | --- |
| cli 与 server 断连、任务仍在跑 | **任务继续跑完**（与现有"连接断开不杀任务"一致），`taskEnd` 缓存在 cli；重连后补发 |
| cli 进程被杀 | server 侧该连接断 → 标 offline；在途任务标记 `lost`，不重试（重试由提交方决定） |
| server 重启 | cli 按退避重连；server 用 `hello` 重建视图；**不恢复**未完成任务（避免重复执行） |
| 任务提交时 agent 忙 | 直接拒绝（`409 busy`），不排队 —— 排队会变成任务黑洞 |

---

## 6. node（`mcp` / `link` 两种模式）与 `remote_ssh`

### 6.1 定位

node 是**可选的远端执行代理**（§1.1）：装在目标机器上，不做决策、不连 LLM。
`link` 模式下它只做"执行一条命令 + 权限审核"；`mcp` 模式下它就是现在那个被控端 MCP server。
**不做运行期增删**：node 列表在 cli 启动时从配置读入并连好，运行期只允许"连不上 → 标 offline"。

两条与"必要性"直接相关的规则：

- `nodes[]` 为空 ⇒ 纯本地执行，**工具表里不出现 `remote_ssh`**（不注册空工具、不给模型一个永远报错的入口）；
- 挂了 node 之后，本地工具**照旧存在**——本地与远端是同一个工具的两个目标，不是二选一。
  这也是"本地终端 + ssh"的自然模型，不需要再造概念。

### 6.1.1 验证面跟随执行面（否则必然假失败）

| 任务执行在 | `verify_cmd` 在哪跑 | 结果 |
| --- | --- | --- |
| local（默认） | cli 本地（现状 `runVerifyAsync`，`cwd=rootDir`） | 正常 |
| node `line1` | **必须也在 `line1`**（走 `remote_ssh`） | 正常 |
| node `line1` | cli 本地 | ❌ 本机没那些文件，验证永远失败 |

⇒ `/task` 与 `taskSubmit` 都要带 `verify_host`（默认 = 执行 host）。**没有 `verify_host` 的远端执行不予支持**，
宁可报错也不要"跑完改对了却判失败"。

### 6.1.2 node 的两种模式（2026-11-06 定）

**角色是固定的：node 永远是 server，cli 永远是 client。**

| 模式 | 起什么（server 角色） | 谁连它（client 角色） | 用途 |
| --- | --- | --- | --- |
| **`mcp`（默认，单独用）** | `JZMcpServer::listenHttp` ⇒ `/mcp`，工具面**与现状一致**（`terminal_*` + 文件工具） | `code_agent` / 任意 MCP 客户端 | 独立被控端，不改现有用法 |
| **`link`（配合 cli）** | **不起 MCP**；起 `JZNetServer` + token，只处理 exec 包 | cli 的 `JZNetClient` | `remote_ssh` 的执行后端 |

配置 `mode: "mcp" | "link"`，默认 `mcp`（向后兼容，现有部署升级后行为不变）。
两种模式**只有一个在跑**，不会同时监听两个端口。

整条链路上只有 cli 是 client，另两端都是 server，这是刻意的对称：

```
   server（JZNetServer）  ◄── JZNetClient ── cli ── JZNetClient ──►  node（JZNetServer）
                     控制面                             执行面
   node（JZMcpServer）    ◄── MCP client ── code_agent / 第三方       ← mcp 模式时才存在
```

- 好处一：cli 只需要"会连"，两端都不需要知道对方的地址（server 不用配 cli 的 IP，node 也不用配 cli 的 IP）；
- 好处二：`link` 模式与 `mcp` 模式对 node 而言只是"换一个 server 实现"，审核/执行那套代码完全共用。

两条硬规则：

- `link` 模式**没有 tools/list、没有 MCP 断连语义**，只有一个隐含的 `exec`；
- **"多节点"这件事只在 cli 侧存在**：cli 连多少个 node 由 `nodes[]` 决定，node 自己不知道有兄弟节点。
  这正是放弃"每个 node 一个 MCP server"的原因 —— 多台 MCP server 在模型侧会变成
  `node1__run` / `node2__run` 一堆名字，还得让模型自己挑；收到一个 `remote_ssh(host=...)` 后
  路由完全在 cli 内部完成，模型只看见一台"虚拟的 ssh"。

### 6.1.3 连通性（node 能不能放在无网机器上）

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
remote_ssh(host: string, command: string, cwd?: string, timeout_ms?: number)
  -> { exit_code, stdout, stderr, timed_out, duration_ms, cwd }
```

| 参数 | 说明 |
| --- | --- |
| `host` | `nodes[]` 里的 `name`；空 = 唯一/默认节点 |
| `command` | **整条交给 shell**（`\|`、`>`、`&&`、通配符、`$VAR` 都生效） |
| `cwd` | 工作目录（相对路径按该 node 的 `rootDir` 解析） |
| `timeout_ms` | 超时，默认 120s，上限 15min（与本地 terminal 前台一致） |

返回里带 `exit_code` 与分流后的 `stdout/stderr`：**没有常驻 shell**，`cd`/`export` 不跨命令
（与 node 现有语义一致，见 `JZAgentNode::buildInstructions`）。

### 6.3 为什么在 cli 侧收成一个工具

| 做法 | 模型看到 | 问题 |
| --- | --- | --- |
| 每个 node 一个 MCP server，工具直接挂进 agent | `node1__terminal_run`、`node2__terminal_open`… | 工具表爆炸；模型要自己挑节点；`terminal_open` 这类会话管理会被误用 |
| **cli 注册一个 `remote_ssh`，内部按 host 路由** | 1 个工具 | 节点对模型不可见，挑选由人（配置的 default）或 `host` 参数决定 |

`link` 模式下 node 只实现 `exec`，所以工具语义天然对齐；`mcp` 模式的工具面**保持不变**（给别的 MCP 客户端用，
不为了这次改动做减法）。

**代价（要认）**：link 模式失去 `background=true` 与流式日志（那是 `terminal_run` 的 MCP 进阶用法），
长命令只能靠 `timeout_ms` + 事后 `tail`。若确实需要，后续加 `remote_ssh_bg` 或返回"最后 N 行"，**第一版不做**。

### 6.4 cli→node 传输：JZNet（`NodePack_*`）

与 server↔cli 同一套框架（`JZNetClient`/`JZNetServer` + token 握手加密），只是包类型与端口不同：

| 方向 | 包 | 字段 |
| --- | --- | --- |
| cli→node | `NodePack_hello` | `clientId, cliVersion`（node 侧做日志/审计；不做路由） |
| cli→node | `NodePack_exec` | `runId, command, cwd, timeoutMs` |
| node→cli | `NodePack_execResult` | `runId, exitCode, stdout, stderr, timedOut, durationMs, cwd` |
| cli→node | `NodePack_execCancel` | `runId`（任务被 `/reset` 或超时时发） |
| node→cli | `NodePack_helloAck` | **node 自述**：身份 / 能力位 / 运行态，字段见 §6.6 |
| node→cli | `NodePack_pong` | 连通性自检（管理台显示 online/offline） |

实现要点：

- **并发**：v1 每个 node **同时只跑 1 条 exec**（`maxConcurrent=1`），cli 侧排队 —— 设备上并跑几条编译只会互相拖死；
- **超时双保险**：cli 侧 timer 到点发 `execCancel` 并本地收口；node 侧也用 `timeoutMs` 兜底杀进程树；
- **大输出**：单包上限 256KB ⇒ `execResult` 里 stdout/stderr 各按"头 8KB + 尾 8KB + `...[截断 N 字节]...`"裁剪；
  确需全文就先 `tail`/重定向到文件再分段读（模型侧做法），不在协议层做分片——**第一版刻意保持简单**；
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
| `maxConcurrent` | 仅 `link`：同时允许几条 exec（默认 1，设备上别调大） |

其余字段（区域、命令策略、shell、会话回收、日志）两种模式完全一致。设计上的两条约束：

- `allowTerminal/allowFileTools` 在 `link` 模式下不再影响"工具面"（没有 MCP 工具），但**仍作为执行策略**保留：
  `allowTerminal=false` 时 exec 一律拒绝；
- `rootDir/trustedDirs/unrestricted` 等区域与命令策略**完全复用** `JZVirtualTerminalConfig`，link 与 mcp 两条路共用一套审核代码
  （`JZAgentNodeHost::refresh` 已经是这个结构，不需要为 link 写第二套）。

### 6.6 node 自述：`helloAck` 该带什么（设计，未实现）

**只回一个 `nodeName` 不够。** cli 和模型要回答三类问题，缺任何一类都会导致"明明连上了却一直撞墙"：

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
| `shell` | string | 终端实际探测 | 人 / 模型 | 命令写法（`sh` / `bash` / `pwsh`） |
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
（`JZAgent.cpp:628-634`），所以 `JZRemoteSshTool` 在 `helloAck` 后调一次 `setToolDefine()`，**下一轮 LLM 请求就生效**，
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

在现有 `{name, target, online, busy, nodeName, rootDir, shell, error}` 基础上补齐上面全部字段，
并增加 `protoVersion`、`platform`、`hostName`、`version`（构建号）、三个能力位、`unrestricted`、
`maxConcurrent`、`running`、`startTimeMs`、`diskFreeMB`。

管理台按 `online + running + lastError + unrestricted` 排序：**需要人看的三类**（离线 / 有 error / `unrestricted=true`）永远排前面。

#### 6.6.4 落地位置

| 改动 | 文件 |
| --- | --- |
| `helloAck` 字段扩到上表 | `JZAgent/JZAgentNodePacket.{h,cpp}` |
| 填字段（构建号、平台、主机名、盘空间、运行态） | `binary/agent_node/JZAgentNode.cpp`（`onLinkPacket` 的 hello 分支） |
| 存字段 + 暴露 getter | `binary/agent_cli/JZNodeLink.{h,cpp}` |
| `helloAck` 后刷新工具描述 | `JZAgentCli::startNodes` 的回调里调 `JZRemoteSshTool::refreshNodeList()` |
| 扩充 `/info` | `JZAgentCli::makeInfo` |
| 单测 | 扩 `test/test_remote_ssh_nodes.py`：断言 `/info` 里有 `platform`、`running=0`、`diskFreeMB>0`，且工具描述里出现"禁装包/禁联网" |

---

## 7. 配置（schema）

> §7.1 / §7.2 已落地（P1/P2，2026-11-06）；§7.3 是 standalone（一直在跑）；
> §6.5 的 node 配置一半是现状（`mcp`）/ 一半已落地（`link`）。
>
> 实际生效的字段以实现为准（`--check` 打印的就是它）：server 侧是 `hub.bind/port/token`，
> cli 侧是 `server.host/port/token/agentId`（**没有** `server.enabled`，`host` 非空即 managed）。

### 7.1 server `config.json`

```json
{
  "http":      { "bind": "0.0.0.0", "port": 8088, "token": "<管理台口令>" },
  "hub":       { "bind": "0.0.0.0", "port": 8090, "token": "<控制通道口令>" },
  "frp":       { "server": { "enabled": false, "bindPort": 7000, "token": "" } },
  "agents": [
    { "agentId": "a1", "token": "t-a1", "name": "产线1号" }
  ]
}
```

| 字段 | 说明 |
| --- | --- |
| `hub.bind` / `hub.port` | `JZNetServer` 监听（默认 8090），控制面专用 |
| `hub.token` | 控制通道握手口令（全局；per-agent token 见 §8） |
| `agents[]` | **v1 遗留**（本机子进程托管），与 hub 并存；hub 侧不靠它做白名单（见 §13.3 的已知缺口） |

### 7.2 cli `config.json`（managed）

```json
{
  "server": { "host": "10.0.0.5", "port": 8090, "token": "t-a1", "agentId": "a1" },
  "nodes": [
    { "name": "line1", "host": "127.0.0.1", "port": 7500, "token": "", "default": true }
  ],
  "llm": { "...": "不变" },
  "terminal": { "...": "不变（本地工作区/权限）" }
}
```

`server.enabled=true`（或 `server.host` 非空）即 managed：**忽略** `http.*` 与 `frp.client.*`，
启动时打一条 warning 说明"已按 managed 模式禁用本地 HTTP/frp"。

`nodes[]` 每项对应一个 `link` 模式的 node（node 侧 `mode:"link"`、`host/port/token` 与这里一致）；
url 不再是 `/mcp`，而是 `host:port` —— 走 JZNet，不是 HTTP。

### 7.3 cli `config.json`（standalone）

`http.*` + 可选 `frp.client.*`，`nodes[]` 取代 `mcp.servers[]`。standalone 也可以挂 node
（本地执行 + 远端执行并存），是否 managed 只影响控制面。

cli 配置分组（两种模式共用，managed 下忽略 `http.*` 与 `frp.client.*`）：

| 分组 | 关键项 |
| --- | --- |
| `llm` | `url / name / token / netTimeoutMs / maxRounds / maxToken / maxRetries / maxFixRetries / retryDelayMs / body` |
| `terminal` | `rootDir / trustedDirs / shellProgram / programs / recycleSec / allowTerminal / allowFileTools / allowPackageManager / allowNetwork / unrestricted` |
| `verify` | `maxRounds / timeoutMs` |
| `memory` | `enabled / file / maxBytes` |
| `system` | `extra / extraFile`（拼进 system prompt 一次） |
| `network` | `enabled / searchUrl / proxyHost / proxyPort / proxyUser / proxyPassword` |
| `mcp` | `enabled / servers[]`（仅第三方 MCP server，不再用于 node） |
| `nodes` | `[{name, host, port, token, default}]`（link 模式 node，静态） |
| `http` | `bind / port / token / dir` |
| `frp.client` | `enabled / serverAddr / serverPort / token / proxies[]` |
| 顶层 | `agentName / version / logFile`（`logFile` 非空才有 `Agent.log`） |

### 7.4 兼容

- 旧 `mcp.servers[]` **保留可读**，但**只服务于第三方 MCP server**（不再是 node 的连接方式）。
  ⚠ 不做"旧 `http://node:7500/mcp` 自动转 `nodes[]`"：传输已经从 MCP 换成 JZNet，自动转会把 node 打成
  连不上的 `mcp` 模式——宁可启动时打一条 warning 说"node 请改用 `nodes[]` + node 侧 `mode:"link"`"，也不要静默错配。
- 旧 `agents[]{cmd,args,dir}`（v1 本地托管）**保留**为一个独立模式 `type:"local"`，
  避免现有部署一升级就全废；但它与本文的 managed 是两套，文档里分开写。

---

## 8. 安全

| 面 | 机制 | 备注 |
| --- | --- | --- |
| 管理台 / API | `http.token`（`?token=` 或 `Authorization: Bearer`） | 与现状一致 |
| 控制通道 | `hub.token`（握手派生密钥）+ `agentId` 身份 | token 不匹配表现是"包解不开"，靠 `hello` 超时给明确错误 |
| per-agent 吊销 | `agents[].token` 覆盖全局：握手后首个 `hello` 校验 `agentId + token` | 推荐；单机泄漏不影响其它 agent |
| node（`link`） | `token`（JZNet 握手）+ 默认只绑内网 | 无 token 时**拒绝启动** link 模式；`mcp` 模式仍是"无鉴权"，必须靠网络隔离 |
| 模型侧 | node 的区域/白名单/危险命令策略照旧 | `unrestricted=true` 等于交出 shell，文档必须显著标注 |

**node 侧的三层防线**（与 `mcp`/`link` 无关，两条路共用）：

1. **区域**：`rootDir`（读写执行）+ 系统临时目录（读写）+ `trustedDirs`（默认只读），别处一律只读；
2. **命令策略**：程序白名单、`allowPackageManager`、`allowNetwork`；危险命令（dd/sudo/shutdown…）
   **无开关**，一律拒绝；
3. **网络隔离**：`mcp` 模式无内建鉴权，只能靠"绑 `127.0.0.1` / 内网 + 隧道"；`link` 模式有 token，
   但仍建议只绑内网。

⚠ 区域约束是**命令级**的（重定向目标写不了区外），程序自己做什么管不到（`rm -rf ~/x` 拦不住）。
真正的边界只能是沙箱。

审计：server 侧按 `taskId` 记录 `{agentId, 发起人, 起止时间, status, verify_rounds}`，落盘一份 JSONL。

---

## 9. 落地步骤

| 阶段 | 内容 | 验收 |
| --- | --- | --- |
| **P0** ✅ 已落地 | node 加 `mode:"link"`（起 `JZNetServer` + `NodePack_exec`，**不起 MCP**）；cli 加 `nodes[]` + `remote_ssh` 工具（`nodes[]` 空则不注册） | `ctest -R agent_cli.remote_ssh_nodes`：2 台 link node + 1 个 cli，把 nodeA 的 secret 搬到 nodeB；`mcp` 模式行为不变 |
| **P1** ✅ 已落地 | cli 模式二选一：`server.host` 非空判定 managed，managed 时不起 HTTP/frp | `ctest -R agent_cli.hub_nodes` 断言 cli 的 `http.port` **未被监听**；standalone 行为不变 |
| **P2** ✅ 已落地 | 控制通道：`JZAgentHubPacket` + `startServerLink()` + server 侧 `JZNetServer`/注册表 + `hello/heartbeat/taskSubmit/taskCancel/reset/taskEvent/taskEnd` + `POST /api/tasks` | 内网 cli 不配 frp，server 能下发任务、看事件、取消任务（`agent_cli.hub_nodes` 全链路通过） |
| **P3** | 收尾：`logAppend` 落盘、离线策略、审计 | 断电/重连/离线各场景行为可预期 |
| **P4**（可选） | node 自述（§6.6）：身份/能力位/运行态 + 刷新 `remote_ssh` 描述 | `/info` 里有 `platform`、`diskFreeMB`；工具描述里出现"禁装包/禁联网" |

包定义建议分两个文件，互不耦合：`JZAgentHubPacket`（server↔cli，控制面）与 `JZNodePacket`（cli↔node，执行面）。
P0 只依赖后者，P2 只依赖前者 —— 这也是 P0 能先落地的原因。

依赖关系：P0 与 P1 互不依赖，可并行；P2 依赖 P1（否则 managed 分支没有接收方）；P4 只依赖 P0。

---

## 10. 与当前实现的差异（落地清单）

> ⚠ 下表是 **P2 之前**的快照。P1/P2 已于 2026-11-06 落地（见 §13）：其中"server 管 agent / 控制通道 /
> cli 模式 / 任务入口 / 取消重置"五行**已完成**；仍然成立的是 **验证面（`verify_host`）、node 自述、日志回传**。

| 项 | 现状（v1） | 目标 | 要改的位置 |
| --- | --- | --- | --- |
| server 管 agent | `JZProcess` 拉本机子进程，配置 `cmd/args/dir` | `JZNetServer` 等 cli 上线，配置 `agentId/token` | `binary/agent_cli/server/JZAgentCliServer.{h,cpp}`、`server/main.cpp` |
| 控制通道 | 无（frp 只是端口转发） | `JZNetClient` + `HubPack_*` | 新增 `JZAgentHubPacket.{h,cpp}`；`JZAgentCli` 加 `startServerLink()` |
| cli 模式 | 总是 `startHttp()`；frp 独立开关 | `managed` 时不起 HTTP/frp | `binary/agent_cli/main.cpp`、`JZAgentCli::init/startHttp` |
| 任务入口 | cli `POST /task` | managed 下为 server `taskSubmit` | `JZAgentCli::submitTask`（复用，不改逻辑）+ server 新路由 |
| 取消/重置 | `POST /reset` | `taskCancel` / `reset` 包 | `JZAgentCli::resetSession` 加一层包入口 |
| **验证面** | 永远在 cli 本机跑（`runVerifyAsync` + `cwd=rootDir`） | `verify_host` 跟随执行 host；远端执行时验证也下发到 node | `JZAgentCli::runVerifyAsync` 增加"走 node"分支；`/task`/`taskSubmit` 加字段 |
| node 角色 | 只有一种：MCP server（8 个工具） | 两种：`mcp`（同现状）/ `link`（无 MCP，只处理 `exec`） | `binary/agent_node/main.cpp`（按 `mode` 分支）、`JZAgentNode`（link 下不建 `JZMcpServer`） |
| node 工具面 | 8 个工具（5 terminal + 3 file）直接进模型工具表 | cli 侧只挂 1 个 `remote_ssh`；`mcp` 模式的工具面不变 | `JZAgentCli::setupAgent`（按 `nodes[]` 决定是否加 `remote_ssh`） |
| node 自述 | `helloAck` 只回 `nodeName/version/rootDir/shell`，模型只看得到 `name (host:port)` | 身份 + 能力位 + 运行态共 17 项，且刷新进 `remote_ssh` 描述（§6.6） | `JZAgentNodePacket`、`JZAgentNode::onLinkPacket`、`JZNodeLink`、`JZAgentCli::makeInfo` |
| node 连接方式 | `mcp.servers[]` 静态、MCP over HTTP `/mcp` | `nodes[]` 静态、**JZNet（`NodePack_exec`）** | `JZAgentCli.h`（配置结构）、`loadConfig`、新增 `JZNodeLink` |
| 日志 | server 读子进程 stdout（3000 行环形缓冲） | `logAppend` 回传 + 按 agentId 落盘 | server 侧新存储；cli 侧 log hook |

---

## 11. 未决问题

1. **（已定，2026-11-06）node 传输 = JZNet**：配合 cli 时 node 走 `link` 模式（不起 MCP，`JZNetServer` + `NodePack_exec`）；
   单独用仍是 `mcp` 模式，工具面不变。多节点由 cli 的 `nodes[]` + 一个 `remote_ssh` 承担，node 之间互不知情。
2. **`remote_ssh` 粒度**：一个工具带 `host`（本文推荐），还是每台机器一个工具名？前者工具表干净，后者参数少。
3. **managed 模式是否保留本地诊断端口**（只绑 `127.0.0.1` 的 `/info`、`/events`）？
   完全不起的话，现场排障只能看 server 侧日志；起了又不满足"不对外起 HTTP"的字面要求（需明确"仅环回不算对外"）。
4. **（已定，2026-11-06）node 保留**：目标是"无网/不能装 cli 的机器"的执行出口，`agent_node` 继续维护，
   P0 的 `remote_ssh` 单工具照做。若后续出现"所有机器都能装 cli"的场景，只要保持 `nodes[]` 可空，
   node 自然退化为不使用的可选层，无需专门删代码。

---

## 12. 实现记录（P0，2026-11-06）

### 12.1 文件

| 文件 | 作用 |
| --- | --- |
| `JZAgent/JZAgentNodePacket.{h,cpp}` | 执行面协议：`NodePack_hello / helloAck / exec / execResult / execCancel` + `JZAgentNodePacketInit()`（幂等注册，必须在 `JZNetPackInit` 之后） |
| `binary/agent_node/JZAgentNode.{h,cpp}` | `initLink()`（只建一台终端，**不建 JZMcpServer**）+ `startLinkServer()` + `onLinkPacket()` + `runLinkExec()` |
| `binary/agent_node/main.cpp` | 按 `config.mode` 分支：`link` → `initLink/startLinkServer`；否则原 MCP 路径 |
| `binary/agent_cli/JZNodeLink.{h,cpp}` | `JZNodeLink`（`JZNetClient` 连接 + 重连 + 单条在飞 + 超时收口）+ `JZRemoteSshTool` |
| `binary/agent_cli/JZAgentCli.{h,cpp}` | `JZCliNodeConfig` / `nodes[]` / `startNodes()` / `nodeLink()` / `defaultNodeName()`；`setupAgent` 里按需注册 `remote_ssh`；`makeInfo` 输出 `nodes[]` 状态 |
| `binary/agent_cli/main.cpp` | 解析 `nodes[]`、打印生效节点、启动时 `cli.startNodes()` |
| `binary/agent_cli/test/test_remote_ssh_nodes.py` | 端到端用例（见 12.4） |

### 12.2 实际行为（与 §6 有出入的地方以这里为准）

- **并发**：每个 node 同时只跑 1 条 exec（`maxConcurrent` 默认 1），忙时 `remote_ssh` 直接回
  "节点 X 正忙"；**不排队**（排队会变成任务黑洞）。
- **输出裁剪**：`execResult` 的 output 超过 **120KB** 时按"头 60KB + 一行显式提示 + 尾 60KB"裁剪
  （按 UTF-8 字符边界切，不会切坏多字节字符），远低于 256KB 的包上限。
- **超时双保险**：cli 侧 timer 到点 → 发 `execCancel` + 本地收口；node 侧 `run(timeoutMs)` 到点杀进程树。
  上限 15min（与本地 terminal 前台一致）。
- **token 必填**：`link` 模式下 `token` 为空直接拒绝启动（否则等于把 shell 交给任何能连上端口的人）。
- **cwd**：非空时拼成 `cd "<cwd>" && <command>` 交给终端 —— 终端的解析器本来就支持 `cd X && cmd`，
  并且会按 X 重新审权限，所以没有为它新开 API；含双引号的 cwd 直接拒绝。
- **/info**：`nodes[]` 每项给出 `{name, target, online, busy, nodeName, rootDir, shell, error}`，
  控制台日志是缓冲的时候靠它自检（token 错的表现是 `online=false` + error 文案）。
  v1 的自述只有这几个字段；扩到平台/能力位/运行态的完整设计见 §6.6（未实现）。
- **重连**：`sigDisconnected` → 1/2/4/…/30s 退避；断线时在飞的 exec 以
  "结果未知" 收口，不重放（远端可能已经执行过）。

### 12.3 兼容

- `mcp` 模式**一行没改**：工具面、`/mcp` 端点、现有 `code_agent` 用法全部照旧。
- 旧 `mcp.servers[]` 仍可连第三方 MCP server，但**不会**被当成 node（传输不同，见 §7.4）。

### 12.4 测试

```powershell
ctest --test-dir build -C Debug -R agent_cli.remote_ssh_nodes --output-on-failure
```

- 起 **2 台 link node**（各自 rootDir）+ **1 个 agent_cli**（`nodes[]` 两台），把 nodeA 的
  `secret.txt` 搬到 nodeB 的 `answer.txt`；
- 默认用**内置 mock 模型**（OpenAI 兼容 + **SSE**，固定回两次 `tool_call` + 一次收尾），
  因此不联网、2 秒级、可进快速回归；`--real-llm` 才用真端点（慢）；
- 断言：`/info` 里两台 node `online=true` → 任务 `status=success` → nodeB 文件内容与随机 secret
  **逐字符相等** → 验证命令只读 nodeB 的文件（cli 本地工作区没有它），所以必须真的经 `remote_ssh` 搬运。

⚠ 写 mock 时踩到的两个点（以后再加类似测试别再踩）：
1. **agent_cli 是流式**（`JZAgent::setStreamEnabled(true)`），mock 必须回 **SSE**（`data: {...}\n\n`）；
   回普通 JSON 会在 `sigReadyRead` 里被当 SSE 消费掉，到 `onLLMRequest` 只剩空 body（报 "json解析错误 ... body=[]"）。
2. 请求体用 `Connection: close`、`Content-Length` 发送（不是 chunked），Python `http.server` 按
   `Content-Length` 读即可。

---

## 13. 实现记录（P1 / P2，2026-11-06）

### 13.1 文件

| 文件 | 作用 |
| --- | --- |
| `JZAgent/JZAgentHubPacket.{h,cpp}` | 控制面协议：`HubPack_hello / helloAck / heartbeat / taskSubmit / taskCancel / reset / taskEvent / taskEnd` + `JZAgentHubPacketInit()`（包号 `NetPack_user+101…`，与 node 的 `1000…` 不冲突） |
| `binary/agent_cli/JZHubLink.{h,cpp}` | `JZHubLink`（`JZNetClient` 连接 + hello/心跳 + 重连退避 + 事件/回包上行 + 任务下行回调） |
| `binary/agent_cli/JZAgentCli.{h,cpp}` | `JZCliServerLinkConfig` / `managed()` / `startServerLink()` / `onHubTaskSubmit·Cancel·Reset` / `cancelTask()` / `nodesJson()`；`pushEvent` 同时上行控制面；`makeInfo` 加 `hub` 段 |
| `binary/agent_cli/main.cpp` | 解析 `server.*`；`--check` 打印运行模式；启动分支：managed → `startServerLink()`，standalone → `startHttp()+startFrpc()`；managed 下配了 frp 只打 warning |
| `binary/agent_cli/server/JZAgentCliServer.{h,cpp}` | `startHub()`（`JZNetServer`）+ 在线 cli 注册表 + `POST /api/tasks` / `POST /api/tasks/cancel` + `/api/status`、`/api/agents` 增加 `hub` 与 `online[]` + 管理台"控制面"表格 |
| `binary/agent_cli/server/main.cpp` | 解析 `hub.*`（校验不与 `http.port` 撞）、默认配置模板、`--check` 打印 |
| `binary/agent_cli/test/test_hub_nodes.py` | 全链路用例（见 13.4），ctest 名 `agent_cli.hub_nodes` |

### 13.2 行为（以代码为准）

| 项 | 实际行为 |
| --- | --- |
| 模式判定 | `server.host` 非空 ⇒ managed（**没有** `server.enabled`）；managed 下 `startHttp()`/`startFrpc()` 都不调用 |
| 上线 | cli 连上后发 `hello`（含 `nodes` 清单），3s 内没收到 `helloAck` 就报"token 或端口不对"并断开重连 |
| 身份 | `agentId` 缺省取 `agentName`；同一 `agentId` 重连时旧连接被 `closeClient` 顶掉（管理台不会留两行） |
| 心跳 | 5s 一次，带 `busy/round/taskId/nodes`；忙闲变化时**立刻**多发一次，不等到下个周期 |
| 任务下发 | `POST /api/tasks {agentId, ...}`，body 与 `POST /task` **完全一致**（`task_timeout_ms` 只给 server 用，不下发语义） |
| 忙 | agent 忙时 server 直接 `409`（带当前 `taskId/round`），**不排队** |
| 回包 | cli 的 `respondTask` 结果经 `taskEnd` 回来，server 用它回 `200`（同步等待式，与 `POST /task` 手感一致） |
| 取消 | `POST /api/tasks/cancel?taskId=` → `taskCancel` → cli 走 `cancelTask()`（与 `POST /reset` **同一条路**，含停验证进程/清后台命令）→ 提交方收到 `status:"cancelled"` |
| 超时 | server 侧 timer 到点：先发 `taskCancel`，再给提交方 `504`（避免 cli 继续跑一个没人收结果的任务） |
| 掉线 | cli 断连 → 该 agent 的在飞任务立刻 `502`（结果未知，不重放）；任务本身在 cli 侧照跑 |
| 事件 | cli 的每个 SSE 事件同时以 `taskEvent` 上行（`thinking/reply/tool_call/tool_result/verify/error/task_start/task_end`），server 存最近一条供 `/api/status` 展示 |
| 单包上限 | 事件 >96KB、回包 >160KB 时换成 `{truncated,bytes,preview}`（**不硬切 JSON**，切坏会让 server 解析失败） |

### 13.3 已知缺口（P3 待办）

- **没有 `logAppend`**：server 侧看不到 cli 的控制台日志；managed 模式下 cli 也没 HTTP，现场排障只能看 cli 自己落盘的 `Agent.log`。
- **没有 agent 白名单**：server 不认识"预期上线名单"，任何拿到 `hub.token` 的 cli 都能连上（token 是全局单一口令）。per-agent token 见 §8，未实现。
- **管理台没有事件流**：只有 `lastEventType`（最近一条），看不到任务过程；要看得加 SSE 转发。
- **`/api/tasks` 是同步等待**：长任务会占着 HTTP 连接；提交方断开后任务照跑（与现有 `/task` 一致），但结果只留在 cli。
- **验证面仍未跟随执行面**（§1.2 / §6.1.1）：`verify_cmd` 依旧在 cli 本机跑 —— 全链路用例里 nodeA/nodeB 与 cli 同机所以成立；真正跨机时必须加 `verify_host`。

### 13.4 测试

```powershell
ctest --test-dir build -C Debug -R agent_cli.hub_nodes --output-on-failure
```

- 拓扑：2 台 link node + 1 个 managed cli + 1 个 server；任务从 **server** 的 `POST /api/tasks` 提交；
- 断言：server 的 `online[]` 里有这台 cli 且它上报的两台 node 都 `online` → 任务 `status=success` →
  nodeB 的 `answer.txt` 与随机 secret 逐字符相等 → 有 `lastEventType`（事件上行通）→ cli 的 `http.port` **未被监听**（模式互斥）；
- 全程 mock 模型，不联网，实测 **0.6s**；standalone 的 `agent_cli.remote_ssh_nodes` 依旧通过（未回归）。
