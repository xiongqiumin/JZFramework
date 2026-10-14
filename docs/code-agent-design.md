# Code Agent 设计（轻量化重构：去 LSP、去调试器、渲染窗独立）

> 2026-09-26 首次成文。背景：现有 `binary/code_agent` 的完整能力盘点之后，决定做**减法** ——
> 面向"小项目、一次成型、客户只看结果"的场景，砍掉两类重资产：
>
> 1. **去掉 `JZTsLangServer`（tsserver/tsgo LSP 客户端）**：小项目不需要常驻语言服务；
> 2. **去掉 `JZRuntimeDebugger`（调试器）**：不给人做单步调试，让 AI 自己打日志、跑、看日志。
>
> 同时把**界面渲染独立成单独进程/窗口**，客户只看到它，看不到 Agent 的对话框。
>
> 本文档是**设计稿**：只写目标、边界、结构与取舍，不含实现代码。与现有实现不一致的地方
> 按"目标态"描述，落地时以本文档为准。

---

## 1. 目标与边界

**做**：

- 一个面向小项目的 JS/TS 生成与运行闭环：**改代码 → 编译 → 运行 → 看日志/截图 → 再改**；
- 语义正确性只靠一个真源：**`tsc` 全量诊断**（编译器说对就是对）；
- 可观测性只靠一条通道：**运行日志 + 未捕获异常（带 source map 翻译回 TS）**；
- 客户可见物只有一样：**独立渲染窗口**（打包后是可双击的独立程序）。

**明确不做**（本次减法的红线）：

| 不做 | 原因 / 替代 |
| --- | --- |
| ⛔ LSP / 语言服务（补全、悬停、定义跳转、符号树、语义重命名） | 小项目靠 `read_file` + `search_context` + 整文件重写；语义正确性由 tsc 兜底 |
| ⛔ 断点 / 单步 / 栈帧 / 作用域取值 / 运行时求值 | 让模型自己插日志再跑（`log → run → read log` 循环） |
| ⛔ 热重载 | 直接重启渲染进程，冷启动成本可接受 |
| ⛔ 多工作区 / 增量编译守护 | 每次 `tsc -p` 全量编译，小项目毫秒级 |
| ⛔ 沙箱容器（AppContainer） | 先用可信目录白名单 + 进程隔离，不做 OS 级沙箱 |

**规模前提**（超过就要重新评估，见 §7）：单工程 ≤ 约 20 个源文件 / ≤ 约 3000 行。

---

## 2. 总体结构：两进程，两种观众

```
        ┌───────────────── code_agent.exe（开发者看） ─────────────────┐
        │  对话 / 任务 / 设置 / 运行日志 / 诊断 / 渲染窗缩略预览         │
        │                                                              │
        │   JZCodeAgent ── 工具集 ──┬─ 文本工具（read/write/modify/search）│
        │                          ├─ tsc_check（唯一语义闸门）           │
        │                          ├─ build_project / run_js             │
        │                          └─ screenshot（抓渲染窗）             │
        └───────────┬──────────────────────────────────┬───────────────┘
                    │ 进程调用（一次性，非守护）          │ 管道 / UI 通道
                    ▼                                  ▼
            ┌──────────────┐                  ┌──────────────────────┐
            │ tsc（外部）   │                  │ code_runtime.exe     │
            │ 编译 + 诊断   │                  │  ★ 客户唯一可见窗口   │
            └──────────────┘                  │  跑 build/*.js        │
                                              │  console/异常 → 回传  │
                                              └──────────────────────┘
```

要点：

- **观众分离**：Agent 的对话框、任务列表、设置是**内部**的；客户看的是 `code_runtime.exe` 的窗口。
  Agent 提供"仅渲染"模式（隐藏自己的主窗口），演示时桌面上只剩成品 UI。
- **崩溃隔离**：渲染进程挂掉不影响 Agent；退出码 + 异常文本回传即可定位。
- **发布即自包含**：导出目录里 `exe + build/ + js_wrapper/`，双击即运行，与 Agent 无关。
  图标/改名/调试开关已由 `JZExeResource`（`JZRT_CFG/DEBUG` 资源）支持，发布时写 `debug=false`。

---

## 3. 去掉 `JZTsLangServer` 之后：语义能力怎么补

### 3.1 裁剪后的工具集

| 现状工具 | 处置 | 替代方案 |
| --- | --- | --- |
| `ts_source` / `ts_quickinfo` / `ts_outline` / `ts_members` / `ts_symbols` / `ts_references` / `ts_definition`（7 个查询） | **删** | `read_file`（整文件）+ `search_context`（grep） |
| `ts_summary`（工程总览） | **改造**（不再走 LSP） | 新增 `project_tree`：文件树 + 每文件行数 / 摘要首行，纯文件系统扫描 |
| `ts_add_class` / `ts_add_function` / `ts_rename_class` / `ts_rename_function` / `ts_replace_function` / `ts_delete_class` / `ts_delete_function`（7 个符号级编辑） | **删** | `write_file`（整文件重写，主力）/ `modify_file`（old_str→new_str，精确块替换）/ 可选 `replace_all` |
| `build_project` | **不给模型**（2026-10-14 定） | 编译由模型**在终端里跑** `tsc -p tsconfig.json`；宿主侧 API `buildProject()` 保留给评测宿主/面板编译按钮 |
| `run_js` | **删**（同一日） | 运行由模型在终端里跑 `code_runtime`（产物目录，缺省入口 `main.js`）；`tsc`/`code_runtime` 以**短名**注册进终端（见 §3.5） |
| `read_file` / `write_file` / `modify_file` / `search_file` / `search_context` / `terminal_*` / `web_*` / `get_date_time` | **保留** | `terminal_*` 是**唯一执行面**（2026-10-14 起 `command`/cmd.exe 那条也已从 code_agent 摘掉，见 §3.5） |
| 路径权限校验（`DenyReason` / 可信目录） | **保留并抽出** | 这块实现值得留在工具库，不随 LSP 一起删 |

### 3.2 三条替代原则

1. **小项目用"整文件重写"，不用符号级 patch。**
   模型输出完整文件内容 → `write_file`。比"定位符号 → 局部替换"更可靠，也不依赖任何语义索引。
   折中：**≤ 约 200 行的文件整文件重写；更长的文件用 `modify_file` 精确块替换**（避免长输出消耗与截断风险）。

2. **搜索即导航。**
   跨文件改名 = `search_context` 全项目列命中 → 逐处 `modify_file`（或 `replace_all` 带命中计数）→
   **立刻跑 `tsc_check`**：漏改会直接报 `TS2304 cannot find name`，编译器就是改名事务的校验器。

3. **代码风格是可改性的前提。**
   工程模板强制约定：顶层符号独占连续行、大括号独占行、成员间空行、统一缩进与 LF/CRLF。
   目的是让 `old_str/new_str` 匹配稳定、让"整文件重写"可预期。

### 3.3 每轮闭环（固定的节奏）

```
模型改码 → write_file/modify_file
        → tsc_check（--noEmit，诊断裁剪后回传）
        → 有错：诊断喂回模型，重改（上限 N 轮）
        → 无错：终端里 `tsc -p tsconfig.json` → `cd build && code_runtime`
        → 收集 stdout + console 日志 + 异常栈（已翻译回 TS）
        → 需要就截图 → 回模型 → 下一轮
```

`tsc` 是**唯一的语义真源**：没有 LSP 之后，"编译通过"就是"改对了"的定义。

### 3.4 代价与补偿

| 损失 | 补偿 |
| --- | --- |
| 没有精确符号定位 | 整文件重写 + 风格约定 |
| 没有语义重命名事务 | 搜索列清单 → 批量替换 → tsc 兜底校验 |
| 没有补全/悬停 | 不需要：写代码的是模型，不是人 |
| 改错了难回退 | 每轮改动前做**工程快照**（`.jz/history/<轮次>/`），替代 LSP 时代的 rename 回滚 |

### 3.5 终端工具（terminal_run / terminal_query / terminal_cancel）—— 契约"不加工"

**职责边界**：终端是**执行面 + 回传面**，不是"信息提炼器"。它只保证三件事：命令照原样跑（白名单/区域审核之外不动手脚）、
输出照原样回、失败可归因。**"哪几行重要"该由模型自己用 `| tail` / `| grep` 决定** —— 工具替它猜，实测只会让它以为输出丢了。

**为什么定成"不加工"（2026-10-14 实测证据）**

| 旧行为 | 实际后果 |
| --- | --- |
| 按"编译/通用"规则**重渲染**输出：每行加 `stdout: ` / `stderr: ` 标签；编译成功**只给尾部 20 行**（且无省略标记）；失败只挑错误行 | 模型看不到预期的那几行 ⇒ 以为命令没跑完/输出丢了 ⇒ **同一条命令重跑 19 次 + `terminal_query` 64 次** ⇒ 撞轮次上限（`MaxRoundError`）判失败 |
| 前台回包尾巴写 `[run_id] …（完整/历史日志用 terminal_query 取）` | 变成"每条命令都去 query 一次"的**钩子**；而 query 的增量语义第二次回"日志 0 条"，模型看不懂 ⇒ 再跑再查 |
| `terminal_query` 标了 `isPollTool()` ⇒ 循环检测**放宽** | 上面那 64 次空转**不会被拦**（放宽本意是"查长任务进度是正常用法"，但"同参数 + 回包完全相同"应当按绕圈计） |

**回包契约**

```
terminal_run  前台:  <stdout/stderr 按到达顺序合并的原始字节>
                     [错误] <原因>       ← 仅当非正常结束（非 0 退出码 / 超时被杀）
                     [提示] <参数被改写> ← 仅当 timeout_ms 被钳到上限 / background 忽略 timeout_ms
terminal_run  后台:  已后台启动(不等它结束, 不设超时) / [run_id] <id> / 用法一行
terminal_run  被拒:  一行原因（没有进程、没有回包体）
terminal_query:      [run_id] <id>  [running|done]  exit=…  pid=…  [后台]  已运行 Ns
                     [command] <命令原文>
                     [日志 N 行(第 a..b 行, 累计 M 行)]      ← 带 grep 时标 grep=…
                     stdout| <行> / stderr| <行>            ← 这里是"看日志"，保留流标签（它不是命令输出本身）
terminal_cancel:     ok / 一行原因（没这条 / 已结束）
```

- `[错误]` 这行**不能省**：`isOk` **不进模型上下文**（`JZAgent` 的 Tool 消息只发 `content`，见 `JZAgent.cpp:1620-1624`），
  不写它模型就无从知道命令失败了（只会看到一段或空白的输出）。
- 前台**不回** `run_id` / `cwd`：后台才有 `run_id`（它是 `terminal_query` / `terminal_cancel` 的唯一凭据）。

**上限：只留"防爆阀"，不留"挑拣器"**（待定，见下）

- 现状只有**内存阀** `kMaxRunOutputBytes = 8MB`（保留末尾、从头部裁、置截断标记）—— 那是防 OOM，**不是上下文阀**。
- ⚠ 框架**不对工具结果做二次限流**（`JZAgent::onToolCallResult` 原样进 history）；超了会触发 `compressHistory()`
  （额外一次 LLM 调用 + 有损摘要 + 10 分钟超时）⇒ "8MB 直接进 prompt"的代价**远大于**"截断 + 显式告知"。
- 建议：**行数为主、字节兜底**（默认 `末尾 500 行 / 32KB，取先到者`，可配 `max_lines` / `max_bytes`）；
  触发时在回包**开头**加一行 `[… 输出过长: 只回末尾 N 行/…B; 完整日志用 terminal_query(run_id, tail=/grep=) 取 …]`，
  内容仍**原样、不重排、不加流标签**。保留**末尾**（错误与结果通常在尾部）；要头部模型可显式 `| head`。

**不做的事**

- 不做流标签重渲染；不做"编译错误上下文"这类重要性挑拣；不替模型判断留头还是留尾；
- 不提供 `error_context` 这种"加工开关" —— **已于 2026-10-14 彻底删除**（`OutputFilter` 枚举 / `setFilterOutput` /
  `isBuildCommand` / `isErrorLine` / `filterBuildOutput` / `truncateGeneric` 连同 `run()` 的 filter 参数一起删掉）；
  要少输出，模型自己 `... | grep -i error | tail -20` —— `grep / head / tail / sort / uniq / wc / cut / sed / awk /
  xargs / tee / diff` 全在程序白名单里（`JZVirtualTerminal.cpp` 的 `kShellCmds`），Git Bash 也自带
  （实测：grep 3.0 + sed/awk/perl 有；`rg`(ripgrep)、`jq`、`python3` **没有**）；
- 不把 `cwd` / `run_id` 塞进前台回包。**例外**：上下文阀触发时那一行标记里带 `run_id`（它是"去哪儿拿全量"的指路，
  不带就等于让模型去重跑命令 —— 那正是要掐掉的环路）。

**已定决策**（2026-10-14 落地）

1. **上下文阀 = 32KB**（`JZVirtualTerminal::setReplyMaxBytes`，默认 `32 * 1024`）：超了只回**末尾**，开头一行
   `[输出过长: 只回末尾 32KB(本次共 N KB, 省略开头 M KB); 完整日志: terminal_query(run_id=…, tail=0) …]`。
   与 `terminal_query` 已有的 32KB 回包上限**同一口径**；**不做**"按重要性挑行"。8MB 内存阀保留（防 OOM，两码事）。
2. **`terminal_query` 默认口径按状态定**：还在跑 ⇒ 增量（轮询省 token，不把同一段日志反复灌进上下文）；
   **已结束 ⇒ 回最后 `tail`(200) 行**（模型这时问的是"跑完了吗/结果是什么"，增量只会回"没有新增输出"，它读成
   "输出丢了"就开始反复查 —— 实测同一个 run_id 查了 64 次）。显式 `from_last` 仍可强制。反复查同一份稳定回包会被
   `JZLoopConfig::pollSameResult = 6`（同签名 + 同结果连续 6 次）兜住 ⇒ **不需要**再改循环检测（放宽阈值本来就只为
   "结果在变"的真轮询留的）。
3. **`command`（`JZShellTool`，Windows 上 `cmd /c`）从 code_agent 摘掉**：它与 `terminal_run` 是**两套 shell 规则**
   （cmd 认 dir/echo，不认 bash 语法、不认 `tsc`/`code_runtime` 短名），实测模型在里面敲 `tsc` 拿到
   `'tsc' is not recognized` 后连试 18+ 次、转而满盘找编译器。类保留在 agent_tool；
   **agent_cli 仍注册着它**（同一产品面的同类问题，未动，待定）。
4. **stdout/stderr 保持合并**（按到达顺序，"原样"）—— 暂无需要区分的用例；真遇到再加 `separate=true` 分两段
   （明确分段，而不是逐行标签）。

---

## 4. 去掉调试器之后：日志驱动的自愈

原则：**不给人做调试，给模型做观测。** 模型缺的不是断点，是"跑起来发生了什么"。

### 4.1 日志通道（必须给足）

渲染进程侧把三类输出**全部**回传宿主（现有能力已够，只需接全）：

- `console.*` → `JZEngineRuntime::setLogHandler`
- 未捕获异常 / 未决 Promise 拒绝 → `setUnhandledExceptionHandler`（**已 source map 翻译回 TS 位置**）
- 进程退出码 / 超时被杀 → 作为一条强信号回报（"超时"比"无输出"信息量大得多）

回传目的地三个：运行日志面板、本轮运行日志文件（`build/logs/run-<时间戳>.log`）、喂给模型的上下文。

### 4.2 日志约定（写进系统提示词）

- 结构化前缀：`console.log("[tag] key=value ...")`，便于 grep 与模型定位；
- **临时日志打标**：调试用的日志统一加 `//@debug` 标记；
- **收尾必须清理**：任务结束前，模型负责 grep 出所有 `@debug` 并删除（或整文件重写一遍干净版），
  交付物里不留调试代码；
- 需要看运行时状态时，鼓励模型**导出一个 `__dump()` 打印关键状态**，而不是向宿主索取"取值原语"。

### 4.3 `log → run → read` 循环

```
终端跑 `cd build && code_runtime` → 收集输出 →（超长只由"防爆阀"兜底, 且带显式标记, 见 §3.5）→ 喂模型
      → 模型判断：信息不够就"加日志"，有 bug 就"改代码"
      → 再跑（上限 3~5 轮；要看增量日志用 terminal_query 的 from_last/tail，避免上下文膨胀）
```

配套：终端跑程序要有**超时**与**输出上限**（上限只是防爆阀，默认值见 §3.5 待定 1），
超时即杀进程并明确回报（`[错误] 命令超时, 已被强制终止`），让模型知道"卡住了"而不是"没输出"。

### 4.4 明确不补的东西

- 断点、单步、栈帧、作用域、运行时求值：**都不做**（`JZRuntimeDebugger` 的 18 个原语整体删除，
  不留 reserved 占位 —— 占位比没有更误导）；
- native crash dump：先不做，靠退出码 + 日志定位；
- 热重载（`js_ui_reload`）：不做，改完直接重启渲染进程。

---

## 5. 界面单独渲染（客户可见）

### 5.1 形态

| | Agent 进程 `code_agent.exe` | 渲染进程 `code_runtime.exe` |
| --- | --- | --- |
| 观众 | 开发者 / 操作者 | **客户** |
| 内容 | 对话、任务、设置、诊断、运行日志、渲染窗缩略 | 被生成的业务 UI |
| 生命周期 | 长驻 | 每次运行一个（改完就重启） |
| 崩溃影响 | 自身挂掉即退出 | 挂掉只回传错误，Agent 继续 |
| 发布 | 不发布 | 改名 + 换图标 + `DEBUG=false`，客户双击即跑 |

### 5.2 交互

- **截图闭环**：渲染窗 → 截图 → 回传模型 → 模型改 UI → 重编译 → 重启渲染进程 → 再截图。
  这是模型"看见自己做的界面"的唯一通道，必须有（`JZScreenShot` 已有能力）。
- **UI 通道只服务截图与最小自动化**（截屏、列举控件），不再承载调试原语。
- **"仅渲染"模式**：Agent 提供开关 / 命令行 `--render-only <工程>`，只弹渲染窗、不弹主窗，演示用。

### 5.3 发布物

导出目录自包含：

```
MyApp/
  MyApp.exe        （由 code_runtime.exe 改名换图标而来，JZRT_CFG/DEBUG=false）
  build/           （tsc 产物 + .js.map）
  js_wrapper/      （与源码同构的一棵树：types/ + <包>/types/ + <包>/dist/）
  project.json     （entry / projectRoot / srcRoot / outDir / buildTime）
```

客户视角：**这就是一个普通桌面程序**，没有对话、没有终端、没有调试端口。

---

## 6. 迁移清单（目标态，不含实现）

**删除**

- `JZTsLangServer.h/.cpp`、`JZTsLangTools.h/.cpp`（纯转发壳）；
- `JZTsEditTools.h/.cpp` 的**符号级编辑部分**（其内部的写文件、路径权限校验、UTF/CRLF 处理
  先抽到工具库再删，别一起丢）；
- `runtime/JZRuntimeDebugger.h/.cpp` 及 runtime 里调试端口相关分支（不留 reserved 占位）；
- `test/lsp_test.cpp`；
- `CMakeLists.txt`：`CODE_AGENT_CORE_SOURCES` 去掉上述文件、删除 `code_agent_lsp_test` 目标与
  exe 输出目录列表里的对应项。

**改造**

- `JZCodeAgent`：注销 15 个 `ts_*` 工具；`tsc` 从"轮末把关"升级为"每轮必跑的语义闸门"；
  把运行输出（stdout / console / 异常栈 / 退出码）统一回传；
- 提示词重写：整文件重写优先、改完必跑 tsc、加 `@debug` 日志并在收尾清理、遇到卡死先加日志再看。

**新增**

- `project_tree`（文件树 + 行数，替代 `ts_summary`）；
- `tsc_check`（`tsc --noEmit` 诊断，替代全部语义查询工具）；
- 工程快照 `.jz/history/<轮次>/`（替代 rename 事务回滚）；
- 可选 `replace_all`（带命中计数的批量替换，替代 `ts_rename_*`）。

**补齐测试的优先级**（当前只有 `taskstore_test` 进了 ctest）：`main_test`（端到端：建工程→编译→运行）
与新的 `tsc_check` 应接入 ctest；GUI 层仍不测。

---

## 7. 取舍与风险

| 风险 | 判断 |
| --- | --- |
| 整文件重写在长文件上烧 token | 折中阈值：≤200 行整文件重写，>200 行走精确块替换 |
| 正则搜索误伤（同名局部变量、字符串里的名字） | 先列命中清单再改 + tsc 兜底；不追求"零误伤" |
| 没有 LSP 后大项目不可维护 | 承认边界：规模前提见 §1；超限就重新引入语言服务（`code_agent_core` 保留插入点） |
| 日志驱动在"无输出的 bug"上无效（死循环、静默失败） | 超时回报 + 强制关键路径打点（提示词要求：入口/分支/返回处各一条日志） |
| 调试日志残留进交付物 | 靠 `@debug` 标记 + 收尾清理步骤，最后一轮做一次全量检查 |
| 客户看到的是"另一个 exe"，与 Agent 视觉割裂 | 这本身就是需求（客户不该看到对话窗）；发布物改名换图标后无感 |

---

## 8. 待定

1. `replace_all` 是否作为独立工具，还是让模型多次调 `modify_file`（倾向后者，先不加）。
2. 运行日志是否需要独立面板，或只落盘 + 回传（倾向两者都要，面板用于人，回传用于模型）。
3. 渲染进程是否支持多实例（同时跑两个工程对比）—— 暂定不支持。
4. `--render-only` 命令行参数是否在本次做。
5. 工程快照的保留策略（保留最近 N 轮 / 只在任务失败时保留）。
