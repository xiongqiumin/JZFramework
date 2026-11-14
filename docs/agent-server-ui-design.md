# Agent Server 管理台 UI 设计：页面交互

> 状态：**2026-11-11 现状盘点 + 设计结论，只落文档、未改代码**。
> 触发场景：核对 `http://127.0.0.1:8088/tasks?id=t1794378052086-1` 的页面交互。
>
> 分工：`docs/agent-hub-design.md` §10/§11 是 A2A 协议、任务记录与页面清单的权威；
> `docs/web-ui-design.md` 是 jz-ui 视觉层与样式迁移；**本文只管管理台页面的交互行为与数据流**，
> 不重复协议字段定义和 CSS 规范。
>
> 鉴权尚未接入：客户端证书（mTLS）已于 2026-11-14 移除，应用层 token 待做；UI 目前不承载登录、
> 不输入 token。默认只绑 `127.0.0.1`，本地联调可 `http.ssl.enabled=false` 退回明文 HTTP。

---

## 1. 设计原则（现状即如此，改动不得破坏）

| # | 原则 | 落点 |
| --- | --- | --- |
| 1 | **单一数据模型**：只有 A2A v1.0 `Task{ id, contextId, status, artifacts[], history[], metadata{} }` | `app.js` 的访问器 `meta/stateOf/taskId/taskEvents/...` 全走这一形状，无第二套旧字段 |
| 2 | **一个 Task 一个稳定 URL**：`tasks?id=<taskId>` | 首页、Agent 页、`/new` 最终都汇到这个深链；列表行点击用 `history.replaceState` 写入 |
| 3 | **派活只有一份实现**：`sendMessage()` → `POST /api/tasks`（`returnImmediately:true`） | 追问 = 同 `contextId` 新建 Task，不往运行中的 Task 塞消息，不改 hub 协议 |
| 4 | **实时性按「数据还会不会变」分档** | 进行中 Task 走 SSE；静态台账 / 历史列表手动刷新；首页「进行中」聚合区 1.5s 条件跟随 |
| 5 | **不维护第二套轮询兜底** | SSE 首帧必发完整 Task 快照，断线由 `EventSource` 自动重连补首帧 |
| 6 | **展示有界** | 后端事件保留最后 200 条；前端单条事件 `data` 截 240 字符、各 `<pre>` 限高，大文本不撑爆 DOM |
| 7 | **页面不在 C++ 里** | `binary/agent_cli/server/web/` → 构建时拷到 `<exeDir>/server_web/`；样式改文件即生效 |

---

## 2. 资源与路由

| 路径 | 页面 / 资源 | 说明 |
| --- | --- | --- |
| `/` | `index.html` + `data-page="agents"` | Agent 台账 + KPI 卡 + 进行中 Task 区 |
| `/new` | `new.html` | `SendMessage` 表单 |
| `/tasks` | `tasks.html` | **Task 中心**（本文重点） |
| `/agent` | `agent.html` | 单机台账：`#chat / #tasks / #card` 三个 hash 页签 |
| `/app.js` `/app.css` `/ui/*` | 静态路由 | 一份 `app.js` 按 `body[data-page]` 分派四个页面；`/ui/*` 是共享 jz-ui |

路由注册在 `JZAgentCliServer::startHttp()`：`/api/*` 精确路由优先，`/agent|/tasks|/new` 走
`PageHandler`，其余路径兜底回首页（方便深链）。静态文件 `setCacheMaxAge(0)`（`max-age=0` + ETag），
页面引用带版本 `/app.css?v=5`、`/app.js?v=5` —— **改前端资源要顺手 +1**。

---

## 3. 页面地图与跳转

```
/new    --下发成功--> /tasks?id=<新 Task id>
/       --进行中「看 Task」/ KPI 卡--> /tasks?id=<id>
/agent  --对话气泡 / Task 表--> /tasks?id=<id>
/tasks  --「继续追问（同 contextId）」--> /new?agentId=<id>&contextId=<ctx>
/tasks  --「该 agent 台账」--> /agent?id=<id>
/tasks  --「关闭」--> /tasks（清 URL query，隐藏详情）
```

`/new` 与 `/agent` 页都预填 `contextId`；追问在 `/new` 仍需用户再点「下发指令」，
不是在当前页直接发消息。首页「进行中」区的「取消」按钮与详情里的取消走同一个 `cancelTask()`。

---

## 4. Task 中心页（`/tasks`）交互规格

### 4.1 加载顺序

`fixNav()` → `loadTasksPage()`：

1. `GET /api/status` 的 `known[]` 填 agent 下拉（**仅首次**，`options.length <= 1` 才填）；
2. 从 URL 读 `agentId` / `state` / `contextId` 回填筛选控件（**只读一次，不写回**）；
3. `GET /api/tasks?limit&agentId&state&contextId` → 渲染 7 列表格 + `meta` 计数 + 绑定行点击；
4. URL 有 `id` → `loadDetail(id)` 展开详情。

「刷新」按钮与四个筛选控件的 `onchange` 都重新执行 `loadTasksPage()`，
因此**改筛选 = 整页重载**（含重新按 URL 的 `id` 拉详情）。

### 4.2 筛选栏

| 控件 | 参数 | 取值 / 行为 |
| --- | --- | --- |
| agent 下拉 | `agentId` | 全部 + `/api/status` 的 `known[]` |
| state 下拉 | `state` | 全部 / SUBMITTED / WORKING / COMPLETED / FAILED / CANCELED / UNSPECIFIED（6 态，见 §7 问题 3） |
| contextId | `contextId` | 自由输入，`trim()` 后发出 |
| 条数 | `limit` | 默认 50，范围 1–500；后端 `<=0` 或 `>500` 一律归一为 50 |
| 刷新 | — | 重跑 `loadTasksPage()` |

`nextPageToken` 恒为空 —— **没有分页**，`limit` 就是「取最近 N 条」。

### 4.3 列表

7 列：Task / agent / 状态徽标 / 最近事件 / 耗时 / 提交时间 / 指令前 60 字。
`hide-sm` 列在窄屏隐藏，表格靠 `data-label` 转卡片。

- 整行 `tr[data-task]` 可点 → `history.replaceState('tasks?id=...')` → `loadDetail(id)`；
- **不是 `<a>`**：不能右键新开、复制链接，也没有 `tabindex` / 键盘 Enter 支持；
- 指令列截断 60 字，耗时取 `metadata.costMs`；进行中显示 `…`，无值显示 `-`。

### 4.4 详情区

| 区块 | 内容 |
| --- | --- |
| 头部 `#detailHead` | id + 状态徽标；agent / 耗时 / 轮次 / 起止时间；`contextId` + `status.timestamp` |
| 操作条 `#detailActs` | 「取消 Task」（条件显示）、「继续追问（同 contextId）」、「该 agent 台账」、「刷新」、「关闭」、`#detailMsg` 提示位 |
| 左栏 | 执行过程 / 事件时间线（seq / type / at / data）、交互 History（无则整块 `display:none`）、字段 kv（指令 / 验证命令 / 状态说明 / agent 回复） |
| 右栏 | 产物 Artifacts 全文 `<pre class="jz-log">` |

按钮显隐：

| 按钮 | 条件 | 行为 |
| --- | --- | --- |
| 取消 Task | `isRunning(t)`（SUBMITTED / WORKING / INPUT_REQUIRED / AUTH_REQUIRED） | `POST /api/tasks/cancel?id=`，按钮立即 `disabled`；提示「已请求取消（A2A: 不保证成功）」；随后重新 `loadDetail()` 继续跟随 |
| 继续追问 | 始终 | `new?agentId=...&contextId=...` |
| 刷新 | 始终 | `loadDetail(id, true)` |
| 关闭 | 始终 | 关 SSE、隐藏详情、`replaceState('tasks')` |

`CancelTask` 回 **202 + 更新后的 Task**，状态可能仍是进行中 —— 前端**不能自己把徽标改成 CANCELED**，
必须继续跟到终态。这是 A2A 语义，不是 bug。

### 4.5 状态机与徽标

`STATE_LABEL` 定义 9 态（含 REJECTED / INPUT_REQUIRED / AUTH_REQUIRED），`WORKING` 集合 4 态，
其余视为终态 `isTerminal()`。徽标色：进行中 `info`、完成 `ok`、失败 `fail`、取消/拒绝/需输入/未知 `warn`。
后端 `recordEvent()` 在收到第一个非 `submitted` 事件时把 SUBMITTED 抬成 WORKING。

### 4.6 实时跟随（SSE）

```
进入 tasks?id=<id>
  └─ GET /api/tasks/detail?id=        (GetTask，全量快照)
       ├─ 终态 → 不建流；页面 = 静态快照，只能手动刷新
       └─ 进行中 → EventSource(/api/tasks/subscribe?id=)
            ├─ 首帧 event:task            = 完整 Task 快照（重连也能补齐）
            ├─ event:status_update        = 每条执行事件
            ├─ event:artifact_update      = 终态产物更新
            ├─ event:task (终态)          = 补完整快照后关流
            └─ 任意一帧 → loadDetail(id, true) → 全量重拉 + 全量重绘
                              └─ 见到终态 → close() EventSource + refreshListOnly()
```

- 前端只监听 `task` / `status_update` / `artifact_update` 三个事件名，**不解析帧内容**，
  帧只是「数据变了」的信号，数据仍从 `GetTask` 全量取。这样重连首帧与事件帧可以走同一条路径，
  代价是每帧一次 HTTP + 一次整块重绘（见 §7 问题 2）。
- 服务端 15s 发 `: keepalive` 注释帧；终态 `finishTaskStreams()` 主动关流；
  `es.onerror` 只提示「SSE 断开，浏览器正在自动重连…」，不做手动重连、不降级轮询。
- 浏览器不支持 `EventSource` 时提示「请点刷新看最新状态」。
- 1.5s 条件跟随 `setFollow(live, fn)` 只在首页 `loadRunning()` 和 `/agent` 页启用，
  `visibilitychange` 时页面隐藏即停、回来续上。**`/tasks` 列表本身不自动刷新**：
  新任务 / 状态变化只在「手动刷新」或「详情 SSE 检测到终态」时反映到列表。

### 4.7 有界展示

| 位置 | 上限 |
| --- | --- |
| 后端 `metadata.events` | 最后 **200** 条（`eventSeq` 仍递增；每 20 条 + 终态落盘一次） |
| 时间线单条 `data` | 字符串化后截 **240** 字符 |
| 指令 / agent 回复 `<pre>` | `max-height` 180px / 220px |
| Artifact `<pre>` | `max-height` 320px，`white-space: pre-wrap` |

⇒ 超出部分靠滚动，被裁掉的**前段过程事件不可回溯**（当前取舍：不撑爆 `tasks.jsonl` 与 DOM）。

---

## 5. API 契约（UI 视角）

| 接口 | 谁调 | 回包要点 |
| --- | --- | --- |
| `GET /api/status` | 四个页面的初始化 | `hub` / `known[]`（旧版 `online[]`）/ `uptimeMs`；填 agent 下拉、算 KPI |
| `GET /api/tasks?...` | 列表、首页进行中区、`refreshListOnly()` | `{tasks[], nextPageToken:"", pageSize, totalSize}`；**`totalSize` 是全库记录数，不随筛选变化** |
| `GET /api/tasks/detail?id=` | 详情每次刷新 | 一个完整 A2A `Task`；404 时在 `#detailBody` 显示「读取失败」 |
| `GET /api/tasks/subscribe?id=` | 非终态详情 | SSE `text/event-stream` + chunked；首帧必是完整 Task |
| `POST /api/tasks/cancel?id=` | 首页 / 详情的取消 | 202 + 更新后的 Task；不保证取消成功 |
| `POST /api/tasks` | `/new` / `/agent` 的 `sendMessage()` | `{id, contextId}`，随后跳 `tasks?id=` |
| `GET /api/agent?id=` | `/agent` 页 | `{kind, info, card, tasks[]}`；`offline` / `process` 时禁用输入框 |
| `POST /api/agent/forget?agentId=` | 首页删除按钮 | 仅离线可删，带 `confirm()`；历史 Task 保留 |

不为 UI 验证增加专用接口；所有数据都从这套 `/api/*` 取。

---

## 6. 实测样本（2026-11-11，联调环境）

`http://127.0.0.1:8088/tasks?id=t1794378052086-1` 的 `GetTask` 返回：

| 字段 | 值 |
| --- | --- |
| `id` / `contextId` | `t1794378052086-1` / `ctx-t1794378052086-1` |
| `metadata.agentId` | `server`（内置 agent） |
| `metadata.prompt` | 「现在什么情况」 |
| `status.state` | `TASK_STATE_COMPLETED` |
| `metadata.costMs` | 7903（7.9s） |
| `metadata.eventSeq` / `events` | 435 / **200 条**（末尾 `task_end`） |
| `history` / `artifacts` | 1 条 / 1 个「agent 回复」（812 字） |

页面表现：进入即展开详情；因是终态，**不建 SSE、不自动更新**，时间线显示最后 200 条事件、
右栏显示 812 字回复；要点「刷新」才会重新取数。这条样本也说明流式 `reply` 事件很密
（435 个事件里大部分是逐字回复），是 §7 问题 2 的现实依据。

---

## 7. 已知问题与改进清单（结论，本轮未改代码）

| 优先级 | 现象 | 影响 | 建议 |
| --- | --- | --- | --- |
| **P0** | 「取消」结果和 `es.onerror` 提示写进 `#detailMsg`，但 `renderDetail()` 每次重建 `#detailActs.innerHTML`，紧接着的 `loadDetail()` 又把提示清空 | 取消反馈 / 断线提示一闪而过，用户以为没点上 | 提示位移到 `renderDetail` 不重建的容器，或在重绘后回填状态 |
| **P0** | 每个 SSE 帧全量 `GetTask` + 全量重绘（200 条时间线、多个 `<pre>`） | 流式回复时高频重绘、滚动位置丢失、请求放大 | 对帧做短节流（如 100–200ms 合并），或改为按帧增量追加时间线；**不能取消首帧全量补快照语义** |
| **P1** | state 下拉只有 6 态，缺 `REJECTED` / `INPUT_REQUIRED` / `AUTH_REQUIRED`（`STATE_LABEL` 与 `WORKING` 里都有） | 这些状态无法单独筛选，与「进行中」定义不一致 | 补齐下拉或由 `STATE_LABEL` 生成 |
| **P1** | 筛选只在初始化读 URL、不写回；关闭详情把全部 query 清空；行点击用 `replaceState` | 刷新 / 分享链接无法复现当前视图；浏览器后退不回列表 | 用 `pushState` + `popstate`，筛选写回 URL；关闭详情只删 `id` 参数 |
| **P1** | `showFatal()` 写 `#msg`，但 `tasks.html` / `index.html` 都没有这个元素 | JS 异常 / 未处理 Promise 在这两页只进 console，界面静默 | 统一兜底提示容器（或写到 `#meta` / `#hint`） |
| **P2** | `totalSize` 是全库数，UI 文案写「共 N 条记录，当前显示 M 条」 | 易被误读为「筛选后共 N 条」 | 文案改成「库中共 N 条」，或后端补筛选后计数 |
| **P2** | agent 下拉只在 `options.length <= 1` 时填充一次；列表行不是 `<a>`、无 `tabindex` | 运行中新增的 agent 不出现（要整页刷新）；行不能新开 / 键盘操作 | 每次 `loadTasksPage` 增量同步选项；行内加真实链接或补 `tabindex` + Enter 处理 |
| 已知限制 | 事件只留最后 200 条、单条截 240 字符 | 长任务前段过程不可回溯 | 当前刻意取舍；要做完整 trace 需另立存储，不在本轮范围 |

---

## 8. 改动约束与验收

- 改 `app.js` / `app.css` 后：静态资源 `?v=` **+1**，或直接改运行目录 `<exeDir>/server_web/` 里的文件后强刷；
  C++ 侧不用重编（页面不是内嵌字符串）。
- 源码在 `binary/agent_cli/server/web/`，构建 POST_BUILD 才拷进 `server_web/`：
  改源码验证要重跑构建或手工同步，**别拿旧产物验证**。
- 发布必须整目录带 `server_web/`（含 `ui/`）；缺目录时 `/` 回 500 + 明确提示，刻意不做内嵌兜底。
- 交互改动后手动验收：① 下发一条新任务 → 详情 SSE 实时刷新；② 跑到终态 → 流关闭、列表刷新；
  ③ 取消进行中任务 → 提示可见且不改徽标；④ 改筛选 → 详情保持；⑤ 断线 → 自动重连补首帧；
  ⑥ 窄屏表格转卡片、按钮可点。
- 不为验收新增专用 API 或测试后门；沿用现有 `/api/*` 与 mock LLM 测试装置。

---

## 9. 明确不做

- 不引入前端框架 / 构建链（现为原生 JS + 模板字符串，见 `web-ui-design.md` §10）；
- 不做 SSE 之外的第二套轮询兜底，不做整页定时重绘；
- 不在 UI 里承载鉴权（登录 / token 输入）——鉴权只在传输层；
- 不做无限事件回溯 / 完整 trace 展示（当前 200 条有界是刻意设计）；
- 本轮不改代码：§7 的每一项都要单独确认后再动手。
