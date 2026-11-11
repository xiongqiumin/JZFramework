# JZShell：库接口、agent 调用流程与兼容性基线

> 日期：2026-11-09（原 `jzshell-compat-baseline.md` 已并入本文，作为第二部分）
> 上半部分（1~9 章）：`JZVirtualTerminal` 的底层从「起 `bash -c` 子进程」换成「进程内
> JZShell」，让终端变成**真有状态的 shell**（cwd / export / 变量 / 函数跨命令保留），
> 同时保持 agent 侧「不阻塞、可超时、可取消」的使用体验。只讲接口与流程。
> 下半部分（10~18 章）：`jz_shell` 对 bash 5.2 官方 `tests/` 的实测兼容性基线（数据）。

---

# 第一部分：接口、流程与取舍

## 1. 分层与职责

```
tool call (terminal_run / terminal_query / terminal_cancel)
        │
        ▼
JZVirtualTerminal          ← 编排：单前台任务、运行记录、回放给模型；审核接口只转发
        │  exec / cancel
        ▼
JZShell                    ← 语义：解析 / 展开 / 内建 / 函数 / 子 shell / 算术
        │  持有 JZShellPolicy
        │  start + wait
        ▼
JZProcess                  ← 进程：fork/exec、管道、退出码、kill
```

三层的状态归属要说死：

| 状态 | 归属 | 说明 |
|---|---|---|
| cwd | **JZShell** | 唯一真相；终端只镜像一份给展示/审核用 |
| 环境变量、普通变量、函数、位置参数 | **JZShell** | `export`/`local`/函数定义都留在这里 |
| 在途运行（run_id、输出、日志、退出码） | JZVirtualTerminal | 供 `terminal_query` 拉增量日志 |
| 白名单、区域、危险参数、安全底线 | **JZShellPolicy**（JZShell 持有） | 2026-11-09 从终端整段搬来；终端的 `checkRead/checkWrite/checkExec/resolvePath` 只剩转发（`shell->policy().xxx`） |

## 2. JZShell 现在的库接口

```cpp
class JZShell : public JZObject
{
public:
    explicit JZShell(JZObject *parent = nullptr);

    // ---- 状态 ----
    bool setCwd(const std::string &dir, std::string *error = nullptr);
    std::string cwd() const;
    void setEnvVar(const std::string &name, const std::string &value);
    void unsetEnvVar(const std::string &name);
    std::string envVar(const std::string &name) const;
    std::map<std::string, std::string> envAll() const;
    void addPathDir(const std::string &dir);          // Windows 上通常加 Git 的 usr/bin

    // ---- 执行 ----
    void setCommandTimeoutMs(int ms);                 // 单条**外部命令**的超时
    Result exec(const std::string &line);             // 同步；阻塞到结束
    bool parseOnly(const std::string &line, std::string *error) const;

    // ---- 取消（新增）----
    void requestCancel();                             // 另一个线程置位 → 外部命令被 kill，exec 返回 130
    bool cancelRequested() const;

    // ---- 审核引擎（2026-11-09 起归本类持有）----
    JZShellPolicy &policy();                          // 白名单/区域/危险参数/安全底线
    const JZShellPolicy &policy() const;

    struct Result
    {
        bool ok;             // exitCode == 0 且没有语法/启动错误
        int exitCode;
        std::string output;  // stdout（2>&1 时含 stderr）
        std::string error;   // 语法/不支持/启动失败
        std::string cwd;     // 执行完的 cwd
    };
};
```

能力边界（会直接影响 agent 的提示词）：

- 支持：`$( )`、`$(( ))`、`|`、`&& || !`、重定向、引号、`~`、通配、`if/for/while`、
  函数（`local`/`return`/`$1..$#`）、子 shell、`#!` 脚本。
- 不支持：反引号、heredoc、`case`、`[[ ]]`、数组、`&` 后台、作业控制、PTY/交互程序。

## 3. 关键决策：exec 的同步/异步怎么摆

JZShell 的控制流是**同步**的（`a && b` 必须等 a 的退出码）。真正的"内部异步"要把解释器
改写成状态机或协程，成本远大于收益。所以异步性只放在**最外面那一层等待**上，三种摆法：

| 方案 | run() 是否阻塞 | 状态跨命令 | 改动量 | 风险 |
|---|---|---|---|---|
| **A. 调用线程里同步跑 exec** | 阻塞 | ✅ | 最小 | 命令跑多久就占住 agent 线程多久（长构建会卡住整个 loop） |
| **B. exec 放到专用工作线程**（推荐） | 不阻塞 | ✅ | 小 | 需要一次回调转发；JZShell 必须只被这个线程使用 |
| C. 仍起子进程，解释器换 `jz_shell.exe -c` | 不阻塞 | ❌ | 最小 | 回到"每条命令一个新 shell"，等于没换成真 shell |

**推荐 B**，它正好对应「JZShell 内部保持同步写法，只有等 JZProcess 的那一段被挪到别的线程」：

- JZShell 的代码完全不用改（`runExternal` 里仍是 `waitForFinished`）；
- 阻塞发生在工作线程，主线程（agent loop / server）照常跑；
- 单前台任务由终端保证：同一时刻只有一个工作线程在跑 exec，天然串行，
  `cd`/`export` 的顺序也就确定了。

## 4. agent 调用流程（按 B 展开）

```
JZTerminalRunTool::execute(params)
 │
 ├─ 1. 取参数：command / timeout_ms / background
 │      background=true → 直接拒绝（没有后台任务，也就没有 & 脱离）
 │
 ├─ 2. m_term->run(command, timeoutMs, cb)
 │      │
 │      ├─ 2.1 单前台任务闸门
 │      │      m_task 非空 → 立刻返回错误，附带"正在跑的那条命令原文"
 │      │      （不排队：排队会让模型以为已经在跑，而 cwd 还停在上一条中途）
 │      │
 │      ├─ 2.2 审核（策略层，不执行；引擎是 JZShellPolicy）
 │      │      JZShellParser::parse(command) 出结构
 │      │      → 每段首 token 过白名单 / 目录区域
 │      │      → 危险参数（rm -rf /、dd of=/dev/…）
 │      │      → 重定向目标过写权限
 │      │      → 安全底线（关机/格式化/删工作区外文件）
 │      │      base 一律取 m_shell->cwd()（不再是终端自己猜的目录）
 │      │
 │      ├─ 2.3 建 run 记录：run_id、command、startTime、status=running
 │      │      日志走 RunRecord，terminal_query 靠它做增量读取
 │      │
 │      ├─ 2.4 m_shell->setCommandTimeoutMs(timeoutMs)
 │      │      前台硬上限 15 分钟（<=0 或超限都归到 15 分钟）
 │      │
 │      ├─ 2.5 投递到工作线程：worker->post([=]{ r = m_shell->exec(command); })
 │      │      ⚠ 只有这一个线程碰 m_shell，串行是天然的
 │      │
 │      └─ 2.6 立刻返回 run_id（JZResult::Ok）
 │
 ├─ 3. 工作线程跑完 → 回主线程收尾
 │      ├─ m_cwd = m_shell->cwd()            ← 状态从 JZShell 镜像回来
 │      ├─ 写 RunRecord：status=done、exitCode、timedOut、error
 │      ├─ 日志按行落 RunRecord（terminal_query 的增量读取游标）
 │      └─ cb(ok, output, error)             ← 工具层在这里 finish(params)
 │
 └─ 4. 回包 = 命令原始输出 + 失败原因一行（不加 [cwd]/[run_id] 之类噪音）
```

失败路径要分清三种，别混成一句"命令失败"：

| 情况 | 谁给的 | 模型看到 |
|---|---|---|
| 审核拒绝 | 2.2 | `命令被拒绝: <program> (<原因>)`，不会执行 |
| 上一条还在跑 | 2.1 | `这个终端同时只能跑一条命令: <原文> 还在跑` |
| 语法/不支持 | JZShell::Result.error | 原始报错（如"不支持 heredoc"），exitCode=2 |
| 命令自己非 0 | exitCode | 输出 + `[错误] 退出码 N` |
| 超时/取消 | 2.4 / requestCancel | `命令超时, 已被终止` / `命令已被取消`，exitCode=124/130 |

## 5. 状态同步规则

改造后有四份"看起来像状态"的东西，必须定死谁跟谁：

1. **JZShell::m_cwd / m_env 是唯一真相**。
2. 终端 `m_cwd` 只在两个时机写：`exec` 收尾时从 `m_shell->cwd()` 拉一次；
   `changeDir()` 时反向推给 `m_shell->setCwd()`。
3. 终端 `m_env` 同理：`setEnv/unsetEnv` 直接转给 `m_shell`，终端不再自己存一份。
4. 审核用的 base 每次从 `m_shell->cwd()` 现取，避免"审核在一个目录、执行在另一个目录"。

> ⚠ 不要在执行线程里回写 policy 的基准目录：`JZShell::exec()` 曾在那里调
> `policy().setBaseCwd(m_cwd)`，而主线程同时在用 `policy().checkExec/resolvePath` 读，
> 结果 `std::string` 数据竞争把 Debug 堆踩坏（实测随机点崩）。基准一律由**调用方显式传
> `base`**（终端就是这么做的）。

## 6. 语义要跟着改的几处（容易漏）

| 位置 | 现在 | 改造后 |
|---|---|---|
| `JZVirtualTerminal::startRun` | 拒绝单独的 `cd`（"每条命令都是新 shell"） | **允许并保留**，这正是真 shell 的意义 |
| 同上 | 拒绝单独的 `export`/`unset` | 允许 |
| `JZAgentCli::buildSystemPrompt` | "每条命令都是一个新 shell；cd/export 不跨命令保留；单独的 `cd` 会被拒绝" | 改成"终端有状态：`cd`/`export` 会保留到下一条命令" |
| 同上 | 告诉模型"是 bash 所以 `[[ ]]`/数组能用" | 改成"内置解释器，POSIX 子集：不支持 `[[ ]]`/数组/heredoc" |
| `runBackground` | 起进程、不设超时 | 明确报错：没有后台任务 |
| `cancel` | kill 进程 | `m_shell->requestCancel()`（跨线程生效） |
| `kill` 工具 | 按 `m_ownPids` 判归属 | JZShell 需要暴露外部命令的 pid，否则只能拒绝 |

> 注（2026-11-11）：第 176/177 两行的文案**已删除**——提示词里不再解释"内建解释器支持哪些语法"，
> 这类常识交给工具描述与报错反馈，避免把篇幅花在模型本来就会写的东西上。

## 7. 接口清单（要动的签名）

**JZShell（新增，已部分完成）**

```cpp
void requestCancel();          // ✅ 已加
bool cancelRequested() const;  // ✅ 已加
```

后续建议补：

```cpp
// 展开后、执行前的审核钩子（审核下沉到真正的 argv，比文本级准）
using Verifier = std::function<bool(const std::string &program,
                                    const std::vector<std::string> &argv,
                                    std::string &why)>;
void setVerifier(Verifier v);

// 外部命令的 pid（kill 归属判定 + 展示）
int lastExternalPid() const;
```

**JZVirtualTerminal（改实现，不改对外语义以外的部分）**

```cpp
// 现在：RunTask 里挂 JZProcess + JZTimer
// 之后：RunTask 只留 run_id/command/output/cb；执行交给 workThread + m_shell
void finishRun(const std::string &runId, int exitCode, const std::string &out,
               const std::string &err, bool canceled, const std::string &reason);
JZShell *m_shell = nullptr;    // 子对象，parent = this
```

## 8. 分期落地

| 期 | 内容 | 验收 |
|---|---|---|
| P1 | 终端内建 `m_shell`；`run()` 在工作线程跑 `exec`；删掉"新 shell"相关拒绝 | 手工 `cd /tmp` → `pwd` 出 `/tmp`；`export A=1` → `echo $A` 出 1 |
| P2 | 收尾走 `finishRun`；run 记录/增量日志/失败分级对齐 | `terminal_query` 能查已完成运行的日志 |
| P3 | 提示词与工具描述改写（有状态 + POSIX 子集 + 无后台） | 模型不再写 `cd x && cmd`、不再用 `[[ ]]` |
| P4 | JZShell `setVerifier`，审核下沉到 argv；暴露 pid | 危险命令在展开后仍被拒；`kill` 恢复归属判定 |
| P4a（已完成 2026-11-09） | 引擎整段搬到 `JZShellPolicy`（`JZShell` 持有），终端的 `check*` 只转发 | `agent_tool` 编译通过；`JZVirtualTerminal` 里再无 `m_config/m_programs/m_trustedPrograms` |
| P4b（未做） | `JZShell::runSimple` 执行前调 `policy()`（展开后的 argv），终端不再自己审 | 绕过终端直接用 JZShell 也拦得住危险命令 |

## 9. 已知取舍（写下来，免得以后争论）

- **没有并发**：一个终端一条前台命令。要并发就多开终端（`JZTerminalRegistry` 本来就是多终端）。
- **没有 PTY**：`vim`/`top`/`python -i` 仍然做不了，这是解释器路线的固有代价。
- **没有 bash 专有语法**：`[[ ]]`、数组、heredoc、进程替换一律不支持，模型提示词必须同步改。
- **超时的粒度变了**：以前是"整条命令一个定时器"，现在是"每条外部命令一个超时"，
  `for i in …; do sleep 1; done` 这种可能整体跑很久但每条都很快 —— 需要在终端层再兜一层
  总时长（工作线程里轮询 `m_shell->cancelRequested()`）。

---

# 第二部分：兼容性基线（bash 5.2 `tests/`）

> 被测对象：`build/test/bin/Debug/jz_shell.exe`（`JZShell/JZShellMain.cpp` 的演示程序）
> 参照对象：Git for Windows 自带的 `bash`（`/usr/bin/bash`）
> 宿主：Windows + Git Bash（MSYS2），无 WSL

## 10. 结论

| 被测 shell | 用例数 | PASS | FAIL | HANG |
|---|---|---|---|---|
| `/usr/bin/bash`（参照基线） | 84 | **50** | 14 | 20 |
| `jz_shell` | 84 | **0** | **83** | 1 |

两个数字要放在一起读：

- bash 的 `14 FAIL / 20 HANG` 基本是 **MSYS2 平台差异**（路径翻译、缺 `rsh`、Windows 版多出
  `shopt completion_strip_exe`、job control/信号/进程替换），不是 bash 的 bug。也就是说，
  这套用例在本机可用的上限约 50/84。
- `jz_shell` 的 `0 PASS` 不是"差一点"，而是**第一个语法/能力检查就过不去**——首轮里绝大
  多数用例在解析阶段就返回了"不支持"。

### 10.1 第二轮/第三轮（补齐能力后重跑）

补了 5 项能力（见 12.2）、再加 `!` 前缀取反后重跑，**后来拿到第 1 个 PASS**，失败的性质也变了：

| 指标 | 首轮 | 第二轮（+5 项能力） | 第三轮（+`!`） |
|---|---|---|---|
| 解析阶段就失败的用例 | 80 / 84 | 10 / 84 | **10 / 84** |
| 能跑完、只是输出不一致 | 4 / 84 | 74 / 84 | **74 / 84** |
| PASS | 0 | 0 | **1**（`run-invert`） |

也就是说：**"跑不起来"已经基本解决，"跑得对不对"才刚刚开始。**

### 10.2 为什么 PASS 一直是 0

bash 的 `tests/run-*` 判定方式是**整份输出逐字节比对**：

```sh
${THIS_SH} ./invert.tests > ${BASH_TSTOUT} 2>&1
diff ${BASH_TSTOUT} invert.right    # 返回 0 才算过
```

所以"通过率"的单位是**文件**，不是命令。`invert.tests` 只有 10 行输出，但它覆盖了
`!` 前缀 ×（简单命令 / 子 shell / 管道）的 10 种组合；`arith.tests` 有 263 行，
`quote.tests` 有 182 行 —— 只要有一处语义不同，整个文件记 FAIL。

而且这些是 bash 团队的**回归测试**，不是入门示例：它们假设 shell 已经完整实现了多行引号、
`$'...'`、反引号、`case`、数组、`set -e` 等一整套语义。所以不存在"简单的 hello world 用例"
—— 最"简单"的 `run-invert` 也要求 `!` + 子 shell + 管道 + 重定向全部正确。

判断进展时，除了 PASS 数，还要看**解析阶段失败的用例数**和 **diff 行数**（见 `rank.sh`）。

## 11. 用例集合

来源：bash 5.2 官方源码包的 `tests/`（`mirrors.aliyun.com/gnu/bash/bash-5.2.tar.gz`；
savannah 走代理连不上）。`tests/` 下有 84 个 `run-*` 驱动脚本，每个形如：

```sh
${THIS_SH} ./arith.tests > ${BASH_TSTOUT} 2>&1
diff ${BASH_TSTOUT} arith.right && rm -f ${BASH_TSTOUT}
```

即：用 `$THIS_SH` 跑 `<name>.tests`，再与 `<name>.right` 逐字节 diff。`$THIS_SH` 换成谁，
被测的就是谁。

### 11.1 两个必须补的前置条件

1. **测试助手 `recho` / `zecho` / `printenv`**
   官方由 `make` + gcc 从 `support/*.c` 编译，本机没有 gcc。
   处理：写独立 C（不 include bash 的 `config.h`），用 CMake + VS2022 `cl` 编成 exe 放到 `tests/`。
   关键点：三个程序都必须 `_setmode(_fileno(stdout), _O_BINARY)`，否则 MSYS2 下输出 CRLF，diff 全错。
   （只补这三个助手，bash 的通过数从 46 → 50。）

2. **`tests/bash`**
   `run-set-x` 的 shebang 是 `#!../bash`（in-tree 构建产物）。没有它就直接 `No such file or directory`。
   处理：用系统 bash 顶替，或把 shebang 改成绝对路径。

### 11.2 jz_shell 的调用约定差异

bash 测试套件用**位置参数**调用被测 shell（`$THIS_SH ./xxx.tests`），而 `jz_shell` 只认
`-c "..."` 和 `--script <文件>`。因此需要一个适配层：

```sh
#!/usr/bin/bash
JZ=/c/.../build/test/bin/Debug/jz_shell.exe
if [ "$1" = "-c" ]; then shift; exec "$JZ" -c "$*"; fi
[ -n "$1" ] && [ -f "$1" ] && exec "$JZ" --script "$1"
exec "$JZ" "$@"
```

`$THIS_SH` 指向这个包装脚本即可。（附带发现：最初的 `jz_shell` 不能执行脚本文件，
见下文错误码 193。）

## 12. 阻塞点分布与补齐过程

### 12.1 首轮：jz_shell 的首个阻塞点

对每个用例采集 `jz_shell` 自己输出的 `[error]` 行（同一用例可能有多条）：

| 数量 | 报错 | 对应能力 |
|---|---|---|
| 33 | `不支持命令替换 `$( )`` | 命令替换 |
| 18 | `语法错误: 多余的 `(``  | 函数定义 `name() { ... }` |
| 11 | `CreateProcess 失败（错误码 193）` | 无法执行 shell 脚本（无 shebang 处理） |
| 6 | `语法错误: 多余的 `)`` | 同上，函数体/子 shell 收尾 |
| 3 | `不支持子 shell `( )`` | 子 shell |
| 2 | `重定向 `>` 后面缺少目标文件` | 紧跟变量的 `>$var` 形式 |
| 2 | `不支持后台运行 `&`` | 后台任务 |
| 2 | `for 后面需要变量名` | 算术 `for ((i=0;i<n;i++))` |
| 1 | `语法错误: 这里需要 `then`, 实际是 `(`` | `if` 里带子 shell/函数 |
| 1 | `不支持 heredoc(`<<`)` | here-document |

另外有 4 个用例（`run-nquote2`、`run-nquote3`、`run-nquote4`、`run-strip`）没有 `[error]` 行，
是纯输出差异——其中一部分来自 `jz_shell` 输出 CRLF（它用 C 文本模式 `printf`），
这部分是**测量工具的问题，不是 shell 语义问题**，把 stdout 改成二进制模式即可消掉。

按解锁用例数排的优先级：

1. `$( )` 命令替换 —— 33 个用例直接卡在这里 —— **已完成**
2. 函数定义 `f() { ... }` —— 24 个用例（18 + 6）—— **已完成**
3. 能执行 shell 脚本（shebang / `.sh`）—— 11 个用例 —— **已完成**
4. 子 shell `( )`、后台 `&`、算术 `for`、heredoc —— 零星

### 12.2 第二轮补齐的能力

| 能力 | 实现要点 |
|---|---|
| `$( )` 命令替换 | 词法阶段按括号配对整体收进词；展开阶段用 `captureCommand()` 真跑一遍，只取 stdout，并回滚 cwd/env/退出码 = 子 shell 语义；内层再解析（支持嵌套） |
| `$(( ))` 算术 | 递归下降求值器：`** * / % + - << >> < <= > >= == != & ^ \| && \|\| ! ~ ?:`、括号、变量（未定义为 0）、十进制/十六进制 |
| 函数定义/调用 | 新增 AST 节点 `Function`；`f() { ... }`、`f ()\n{ ... }`、`f() ( ... )` 三种写法；调用时绑定 `$1..$9`/`$#`/`$@`，结束后还原 |
| `local` / `return` | `local` 记录旧值到保存栈，函数返回时逐个还原；`return` 设置返回标志与退出码 |
| 子 shell `( ... )` | 新增 AST 节点 `Subshell`；执行前后保存/恢复 cwd、env、位置参数 |
| `!` 前缀取反 | `parsePipeline` 先吃掉 `!`（可叠加），`Node::negate` 在 `runNode` 收尾时把 status 取反；作用对象是整条管道 → **拿下第一个 PASS（`run-invert`）** |
| 执行脚本文件 | 目标文件以 `#!` 开头且不含 `\0` 时，用本解释器跑（剥掉 shebang 行），位置参数按 argv 注入，状态隔离。**不再报 CreateProcess 193** |
| 输出可比对 | `jz_shell -q` 关闭 `$ cmd` / `[exit N]` 横幅；stdout/stderr 设 `_O_BINARY`，不再有 `\r` |

对应源码：`binary/agent_tool/JZShell/JZShell.cpp`、`JZShell.h`、`JZShellMain.cpp`。

## 13. 最接近通过的用例（按 diff 行数排序）

`sh build/bench/rank.sh` 会按 diff 行数从少到多列出：

| diff 行数 | 用例 | 还差什么 |
|---|---|---|
| 0 | `run-invert` | **已 PASS** |
| 2 | `run-ifs-posix` | `((...))` 命令形式；但后面还要 `read` + IFS 的 6856 组语义，短期不可能 |
| 5 | `run-posix2` | heredoc `<<`、`case`、`eval` |
| 8 | `run-dynvar` | `case` + `BASHPID`/`SECONDS`/`BASH_ARGV0` 等动态变量 |
| 9 | `run-dbg-support2` | `trap DEBUG` + `shopt`（调试支持） |
| 11 | `run-coproc` | `coproc` |
| 24 | `run-strip` | `echo -n`、反引号 |
| 27 | `run-extglob3` | `[[ ]]` + 扩展通配 |
| 28 | `run-precedence` | `$(( ))` 的优先级细节、`+=` |

结论：`run-invert` 之后没有"再补一处就过"的用例了，最近的三个（posix2 / dynvar /
dbg-support2）各需要 2~3 个子系统。

## 14. 现在还剩什么（74 个用例的实际差异来源）

| 缺失 | 影响的用例举例 |
|---|---|
| ANSI-C 引用 `$'...'` | `run-nquote4`、`run-nquote5`、`run-quote` |
| 反引号 `` `...` `` | `run-quote`、`run-strip`、多数 `*.tests` 的 harness 片段 |
| `[[ ... ]]` 条件表达式 | `run-invert`、`run-cond`、`run-extglob2` |
| `case ... in pattern)` | `run-posixpat`、`run-case`、`run-casemod` |
| `(( ... ))` 命令形式（返回 status） | `run-arith-for`、`run-array` |
| 后台 `&`（函数体内也会用到） | `run-cprint`、`run-jobs`、`run-minimal` |
| `echo -n` / `set -e` / `set -x` / `set -o` | `run-strip`、`run-set-e`、`run-set-x`、`run-shopt` |
| 数组 / 关联数组 / `${!var}` | `run-array*`、`run-assoc`、`run-nameref` |
| heredoc `<<` / here-string `<<<` | `run-heredoc`、`run-herestr` |
| 作业控制、`trap`、`read`、`mapfile` | `run-jobs`、`run-trap`、`run-read`、`run-mapfile` |

结论：**接下来不该继续"铺功能"，而应该挑用例逐个攻**。挑一个目标用例（例如 `run-invert`），
把它需要的语义一次做齐，拿到第一个 PASS，再按用例簇推进 —— 否则一直停在
"跑得动但都不对"的状态。

## 15. 明细（84 个用例）

| 用例 | bash | jz_shell |
|---|---|---|
| `run-alias` | PASS | FAIL |
| `run-appendop` | PASS | FAIL |
| `run-arith` | PASS | FAIL |
| `run-arith-for` | PASS | FAIL |
| `run-array` | HANG | FAIL |
| `run-array2` | PASS | FAIL |
| `run-assoc` | FAIL | FAIL |
| `run-attr` | PASS | FAIL |
| `run-braces` | PASS | FAIL |
| `run-builtins` | HANG | FAIL |
| `run-case` | PASS | FAIL |
| `run-casemod` | PASS | FAIL |
| `run-complete` | PASS | FAIL |
| `run-comsub` | PASS | FAIL |
| `run-comsub-eof` | PASS | FAIL |
| `run-comsub-posix` | PASS | FAIL |
| `run-cond` | FAIL | FAIL |
| `run-coproc` | FAIL | FAIL |
| `run-cprint` | PASS | FAIL |
| `run-dbg-support` | PASS | FAIL |
| `run-dbg-support2` | PASS | FAIL |
| `run-dirstack` | PASS | FAIL |
| `run-dollars` | HANG | FAIL |
| `run-dynvar` | PASS | FAIL |
| `run-errors` | FAIL | FAIL |
| `run-execscript` | HANG | FAIL |
| `run-exportfunc` | HANG | FAIL |
| `run-exp-tests` | FAIL | FAIL |
| `run-extglob` | HANG | FAIL |
| `run-extglob2` | PASS | FAIL |
| `run-extglob3` | PASS | FAIL |
| `run-func` | PASS | FAIL |
| `run-getopts` | PASS | FAIL |
| `run-globstar` | HANG | FAIL |
| `run-glob-test` | HANG | FAIL |
| `run-heredoc` | HANG | FAIL |
| `run-herestr` | PASS | FAIL |
| `run-histexpand` | PASS | FAIL |
| `run-history` | PASS | FAIL |
| `run-ifs` | FAIL | FAIL |
| `run-ifs-posix` | HANG | FAIL |
| `run-input-test` | PASS | FAIL |
| `run-intl` | FAIL | FAIL |
| `run-invert` | PASS | FAIL |
| `run-iquote` | PASS | FAIL |
| `run-jobs` | HANG | FAIL |
| `run-lastpipe` | PASS | FAIL |
| `run-mapfile` | PASS | FAIL |
| `run-minimal` | HANG | HANG |
| `run-more-exp` | FAIL | FAIL |
| `run-nameref` | PASS | FAIL |
| `run-new-exp` | HANG | FAIL |
| `run-nquote` | PASS | FAIL |
| `run-nquote1` | PASS | FAIL |
| `run-nquote2` | PASS | FAIL |
| `run-nquote3` | PASS | FAIL |
| `run-nquote4` | FAIL | FAIL |
| `run-nquote5` | PASS | FAIL |
| `run-parser` | PASS | FAIL |
| `run-posix2` | FAIL | FAIL |
| `run-posixexp` | HANG | FAIL |
| `run-posixexp2` | PASS | FAIL |
| `run-posixpat` | PASS | FAIL |
| `run-posixpipe` | PASS | FAIL |
| `run-precedence` | PASS | FAIL |
| `run-printf` | PASS | FAIL |
| `run-procsub` | HANG | FAIL |
| `run-quote` | PASS | FAIL |
| `run-quotearray` | PASS | FAIL |
| `run-read` | HANG | FAIL |
| `run-redir` | HANG | FAIL |
| `run-rhs-exp` | PASS | FAIL |
| `run-rsh` | FAIL | FAIL |
| `run-set-e` | PASS | FAIL |
| `run-set-x` | PASS | FAIL |
| `run-shopt` | FAIL | FAIL |
| `run-strip` | PASS | FAIL |
| `run-test` | HANG | FAIL |
| `run-tilde` | PASS | FAIL |
| `run-tilde2` | PASS | FAIL |
| `run-trap` | HANG | FAIL |
| `run-type` | HANG | FAIL |
| `run-varenv` | FAIL | FAIL |
| `run-vredir` | FAIL | FAIL |

> 说明：上表的 `jz_shell` 列是**首轮**（未补任何能力）的数据，用来定位阻塞点；
> 补齐后的进展见 10.1 与 12.2，逐轮 TSV 在 `build/bench/results/`（`build/` 被 gitignore）。

## 16. 复现

```sh
# 0) 准备（一次性）
#    - bash 5.2 源码解压到 build/bench/bash-5.2
#    - tests/ 下放 recho.exe / zecho.exe / printenv.exe（CMake + VS2022 编译）
#    - tests/bash -> 系统 bash

# 1) 跑 bash 基线
cd build/bench/bash-5.2/tests
export THIS_SH=/usr/bin/bash TMPDIR=/tmp BASH_TSTOUT=/tmp/bt PATH=.:$PATH
timeout 20 ./run-arith            # 单个用例

# 2) 批量（84 个，结果写 results/<label>.tsv）
sh build/bench/run-matrix.sh bash /usr/bin/bash $(cat /tmp/list.txt)
sh build/bench/run-matrix.sh jzshell /c/.../build/bench/jzsh $(cat /tmp/list.txt)
```

判定：`diff` 返回 0 记 PASS；`timeout` 命中 124 记 HANG；其余记 FAIL。

## 17. 已知测量误差

- **CRLF**：`jz_shell` 用文本模式输出，`\r` 会让每一行都 diff 失败。这会**低估**通过数。
- **MSYS2 能力缺失**：涉及 job control、信号、进程替换、交互 `read` 的用例在宿主上会 HANG，
  对两边一视同仁，但对 bash 来说这些本来应该是 PASS。
- 用例依赖的 `recho`/`zecho`/`printenv` 是自建替身，行为对齐原 C 实现，但未做逐字节回归。

## 18. 下一步

1. ~~stdout 二进制模式~~ 已完成（`-q` + `_O_BINARY`）。
2. ~~`$( )` / 函数 / 脚本执行~~ 已完成，见 12.2。
3. **改为按用例推进**：选一个目标用例（建议 `run-invert`：只需 `$(( ))` + 字符串比较），
   把它需要的语义一次做齐，拿到第一个 PASS 之后再按簇推进。
4. 每轮结束重跑 `run-matrix.sh`，对比 `results/jzshell-vN.tsv` 的 PASS 数。

> 测量提醒：`PASS` 是"整份输出逐字节相同"，补一半语义仍然是 FAIL。
> 判断是否有进展，除了 PASS 数，还要看 **`解析失败` 的用例数**（首轮 80 → 第二轮 10）。

---

# 第三部分：后台任务与进程化决策（2026-11-11）

## 19. 结论

**保留「进程内库调用 + 后台用线程」的现状，暂不把 JZShell 拆成独立 exe 走 IPC。**

| 事项 | 结论 |
|---|---|
| JZShell 与 agent 的关系 | 继续同进程（lib 调用），不做 `--serve` 进程化 |
| `cmd &` 的后台执行 | 外部命令 → 分离进程；函数/复合命令/管道/脚本 → **线程**（模拟 fork） |
| 审核策略归属 | 路径判定只用终端的 `m_config`（静态 `checkReadIn/checkWriteIn`）；程序审调 shell 里的 policy（见 §29）|
| 为什么线程可接受 | CLI 与 code_agent 都是常驻进程，线程生命周期跟随进程；不需要跨进程同步 |
| 什么情况下才回头做 IPC | 需要「shell 崩溃不拖垮 agent」或「agent 重启后会话仍在」时（见 §27） |

## 20. 背景：本来要解决什么

`cmd &` 之前是"当分隔符、同步执行"，等于没有后台。补上之后有两种实现路线：

- **路线 A（进程化）**：JZShell 编译成独立 exe，`JZVirtualTerminal` 通过 `JZLocalSocket` 通信；
- **路线 B（当前采用）**：仍然库调用；后台任务按"能否直接 exec"分档，实在不能的用线程。

## 21. 现在的实现：后台任务分两档

| 写法 | 走哪条 | 有无线程 | pid |
|---|---|---|---|
| `./server &`、`python app.py &`（纯外部命令）| `JZProcess::start()` + `detach()` | 无 | 真实 pid |
| `f1 &`（函数）、`{ ...; } &`（复合命令）、`a \| b &`、`./x.sh &`（脚本文件）| 复制一份 `JZShell` 状态 + `JZTaskThread` | 1 个 | 慢路径没有 pid |

要点：

- 输出的去向**由命令自己的重定向决定**：`cmd > log 2>&1 &` 写文件（边跑边写），没重定向就丢弃
  （父 shell 已回包，那一路没有接收者）；
- `nosuchcmd &` 与 bash 一致：**父 shell** 打 `command not found`，退出码仍是 0（不触发 `set -e`）；
- 慢路径跑完把**没被重定向掉**的 stderr 打到进程 stderr（bash 的后台 stderr 也是终端）。

## 22. 为什么后台不用"新拉一个 shell"

bash 的标准做法是 `fork()`：子进程是**内存镜像副本**，函数表、变量、别名、cwd、选项全在。
如果改成 `jz_shell -c "<原文>"`，等于 `fork+exec` 的语义：

| 状态 | fork（bash 标准） | 新 shell 进程 |
|---|---|---|
| 函数 `f1` | ✅ | ❌ 找不到（除非 `export -f`）|
| 未 export 的变量 `x=1; echo $x &` | ✅ 打印 1 | ❌ 空 |
| 别名 / `set -e` / `shopt` | ✅ | ❌ 丢 |
| cwd | ✅ | ⚠️ 要靠 `--cwd` 传 |

`f1 &` 这种用例，函数体本身就是要执行的东西，新 shell 里根本不存在。Windows 没有 fork，
`RtlCloneUserProcess` 是未文档化 API 不能上生产，所以**线程 + 状态副本**是"内存镜像复制"
的等价替代（Git Bash 走的是 Cygwin 在用户态复制地址空间的 fork 模拟，代价是几十 MB 级拷贝）。

## 23. 为什么暂不进程化（四个硬问题 + 一个前置风险）

接口面其实很小——`JZVirtualTerminal.cpp` 对 `m_shellImpl` 只有 12 处调用，执行的唯一入口是
`sh->exec(cmd)`。真正的成本在这四点：

**P1（硬约束）`policy()` 不能跟着 shell 走。** `JZVirtualTerminal::checkRead/checkWrite`
是给 `JZCodeTools` 的文件工具用的（判断路径能不能读写），它不经过 shell。policy 搬进 exe，
文件工具就失去审核能力。→ **2026-11-11 已拆开：路径判定只用 config（静态函数），程序审仍调 shell，见 §29。**

**P2（安全）`cwd` 不同步会让路径审核错位。** 现在 `checkExec(head, argv, base)` 的 base 来自
终端自己的 `m_cwd`；进程化后 `cd` 发生在 shell 进程里，agent 用旧 cwd 判定 → 可能放行本该
拒绝的 `rm x`。

**P3 输出要改成流式。** 现在 `exec()` 一次性回包，而"超时被杀前保留已输出内容"依赖同一个
对象里的缓冲；进程化后必须有 `{id,stream,data}` 流式回传，否则长命令在超时前什么都拿不到。

**P4 exe 缺失 = 终端全废。** 现在 lib 永远不会缺；改成 exe 后打包遗漏/杀软拦截/路径不对，
`terminal_run` 直接不可用，需要明确的降级策略。

**前置风险：`JZLocalSocket` 还没测过。** 它 Windows 上走 `AF_UNIX`（Win10 1803+）或回退
`127.0.0.1`，嵌入式还会退化成"进程内全局变量"，且没有粘包/断线/大消息的测试。

## 24. 为什么线程方案在常驻进程里是合适的

- `jz_shell`（CLI）和 `code_agent` 都是**常驻进程**，后台线程的生命周期跟着进程走，
  不需要"跨进程存活"这种语义；
- 线程方案让 `f1 &`、`{ ...; } &`、`a | b &` 全部可用，代价只有一条：
  **进程退出时线程可能还在跑**（见 §25 的 `finish()`）；
- 线程里跑外部命令必须用 `JZTaskThread`（`JZProcess` 建定时器时会校验当前线程是
  `JZApplication` 登记的 `JZThread`，裸 `std::thread` 直接 assert 崩溃）。

## 25. 已落实的实现要点

| 位置 | 做法 |
|---|---|
| `JZProcess` | 每路 IO 一个 `ChannelTarget`（`Pipe/File/Handle/Null/Forward`），后设覆盖先设；`setStandardXxxFile(path, Truncate\|Append)` / `...Handle(intptr_t)`（借用，不关）/ `...Null()` |
| `JZProcess::start()` | 尊重上述设置：设了文件/句柄/NUL 的那一路不建管道；**同名文件复用同一句柄**（否则 2>&1 两路各自从 0 写互相覆盖）|
| `JZProcess::detach()` | 纯应用层放手：`killTimer` + 摘掉作业的 `KILL_ON_JOB_CLOSE` + 关句柄；有托管管道时 `canDetach()` 为 false |
| 句柄继承 | `STARTUPINFOEX + PROC_THREAD_ATTRIBUTE_HANDLE_LIST`，只继承三个目标句柄（否则宿主的管道写端被继承，退出时等不到 EOF）|
| `JZShell::spawnBackground` | 快路径 `start()+detach()`；慢路径 `forkForBackground()` + `JZTaskThread` |
| `JZShellMain` | `finish(rc)`：还有后台任务在跑就 `fflush` 后 `_exit(rc)`（`~JZApplication` 断言线程表为空，不能正常析构）|
| `exec` 内建 | 执行完置 `m_exiting/m_exitCode`（bash 的 exec 是替换进程，后面的语句不再执行）|

## 26. 已知限制（写下来，免得以后争论）

- **`wait` 是空操作**：`f1 &; wait; echo $?` 拿不到函数返回值（bash 是 5，我们是 0）。
- **慢路径的后台任务不跨进程**：agent 退出即消失；`terminal_cancel` 也管不到（副本的
  `m_cancelRequested` 是独立的）；它内部起的外部命令仍挂在带 `KILL_ON_JOB_CLOSE` 的作业上。
- **脚本文件后台目前也走慢路径**（`./x.sh &` 应该是一个子进程，语义上更接近 bash，待优化）。
- **`_exit` 绕过静态析构**：有后台任务在跑时进程直接退出，不做全局清理。
- **函数后台的半成品语义**：`$!` 没有值、不能 kill —— 这也是"必要时可以砍掉 `f1 &`"的依据。

## 27. 如果将来要进程化：路线

前置：先补 `test_localsocket`，至少覆盖 echo 往返、4MB 单条消息（粘包）、客户端断开、
服务端被杀、两客户端并发、Windows 强制走 `127.0.0.1` 回退。

```
阶段 0  test_localsocket（6 条）                       ← 没有它，后面都是空中楼阁
阶段 1  抽 IJZShellBackend（exec/cancel/配置下发）
        InProcBackend 包住现有 JZShell → 52 个用例仍须 12 PASS（零回归保险）
阶段 2  OutProcBackend：LocalSocket + 流式输出 + cwd 回传
        同一批用例逐条对比输出（不只看 PASS/FAIL）
阶段 3  删 InProcBackend，JZShell 从 agent_tool 移出，编译成独立 exe 并纳入发布包
```

进程化时必须先回答：

1. **policy 放哪**：A 只留 agent（能力等于现在，P2 的洞补不上）；B 两份 + 配置下发（正确但复杂）。
2. **一终端一进程，还是一进程多会话**：前者最简单（`cwd/env` 天然隔离），后者要 `session_id` 路由。

建议：把"审核下沉到执行层"当成**独立事项**，不要和进程化捆在一起——两个变量同时动，
出问题无法定位。

## 28. 回归基线（2026-11-11 实测）

| 项 | 结果 | 说明 |
|---|---|---|
| bash-5.2 矩阵（48 个 target） | **PASS=12** | 直接以 `jz_shell.exe` 作为 `$THIS_SH`；`results/jzshell-v48.tsv` |
| 逐例退出码扫描（48 个） | 无 rc≥128、无 FATAL | 唯一异常是 `run-func` 超时（用例自身卡住）|
| `test_terminal_tools` | fail=32 | **既有失败**（stash 到父提交后对比一致，非本轮引入）|
| `test_sharedmem` | ALL PASS | 含跨进程 attach |

跑法（**不要把 `jzsh` 包装脚本加回来**：多一层 bash 会改变环境，`run-parser`/`run-lastpipe`
会因此对不上期望）：

```bash
cd build/bench
CAP=20 ./run-targets.sh <label>
# run-targets.sh 自己读 targets.txt, 内部等价于:
# ./run-matrix.sh <label> /c/.../build/test/bin/Debug/jz_shell.exe $(cat targets.txt)
```

## 29. 审核分层：一份 config + 一份 policy（2026-11-11）

### 29.1 问题

同一个 `JZShellPolicy` 之前被两件不相关的事共用：

| 谁在问 | 问的是什么 | 走哪条路 |
|---|---|---|
| `JZCodeTools` 的文件工具 | 这个**路径**能不能读/写（目录边界、读写区域）| 不经过 shell |
| 终端跑命令前 | 这个**程序/参数**能不能执行（白名单、危险参数、安全底线）| 经过 shell |

前者只需要**配置**（zone / permissions），跟程序表、可信程序、危险参数毫无关系；
后者才需要完整的 policy 引擎。

### 29.2 现在的形态

```
JZVirtualTerminal
 ├─ m_config        ← JZVirtualTerminalConfig（只做路径判定，见 checkReadIn/checkWriteIn）
 └─ m_shellImpl     ← JZShell，唯一一份 JZShellPolicy，只给"执行前审"用
```

- **不放 policy 对象**：`JZVirtualTerminal` 只留 `m_config`；
- 文件工具/路径判定走静态函数（不需要程序表）：

  ```cpp
  JZShellPolicy::checkReadIn(m_config, path);
  JZShellPolicy::checkWriteIn(m_config, path);
  ```

- **程序/参数审核直接调 shell**：`policy()` 返回 `m_shellImpl->policy()`
  （`checkExec` / `checkCriticalFloor` / `resolvePath` 都走它）；
- `setTerminalConfig()` 同时做两件事：`m_config = config` + `policy().setConfig(config)`。

### 29.3 验证

| 项 | 结果 |
|---|---|
| `test_code_tools`（文件工具 + 审核）| ALL PASS (fail=0) |
| `test_terminal_tools` | ALL PASS (fail=0)（已按"单前台、无 run_id"重写）|
| 行为变化 | 无（入口审核仍在 `JZVirtualTerminal::run`，判定逻辑同一份）|

### 29.4 还没做的

`JZShell` 里那份 policy 目前**只有配置、没有调用点**（`m_policy` 仅出现于
`setBaseCwd`）。等"审核下沉"（展开 argv 之后再审，覆盖 `$CMD` / alias / `rm *` /
`eval` / `source` / `$( )`）落地时，调用点加在 `runSimple` 展开之后、`runExternal`
之前——那时它才真正开始工作。

### 29.5 顺带删掉的终端接口（同日）

终端不再有"外部 shell"这一层（命令由内建 `JZShell` 直接解释），也不再自持程序表：

| 删掉 | 替代 |
|---|---|
| `setShellProgram()` / `shellProgram()` | 无；固定内建解释器。`terminal.shellProgram` 只给 verify 脚本选解释器 |
| `setTrustedPrograms()` / `trustedPrograms()` | `JZVirtualTerminalConfig::programPaths` |
| `addProgram()` / `availablePrograms()` | 同上：`JZShellPolicy::setConfig()` 按 `programPaths` 补白名单 |

`ack.shell` 与启动日志里的 shell 字段固定上报 `jz_shell(内建)`。

### 29.6 删掉运行记录 / run_id / terminal_query（2026-11-11）

终端本来就是**单前台任务**（一次只有一条命令），"运行记录 + run_id + 增量查询"这套是为
早先的并发模型建的，此后一直空转。现在整条链路删掉：

| 删掉 | 现在 |
|---|---|
| `terminal_query` 工具（本地 / MCP / remote_ssh 的 `action=query`）| 没有。要看日志就让命令自己重定向(`> log 2>&1 &`)再 `cat`/`read_file` |
| `JZVirtualTerminal::run()` 返回的 run_id | 返回 `JZResult<void>`（成功 = 已开始）|
| `runSnapshot` / `runIds` / `advanceRunReadPos` / `activeRunId` / `RunRecord` / `RunRecordInfo` | 全删；只留一份在途 `RunTask`（命令 + 累积输出 + 回调）|
| `terminal_cancel` 的 `run_id` 参数 | 无参：停的就是当前那一条 |
| `RunTask` 按 runId 查找 | 单任务，直接取"唯一那条" |
| `NodePack_termQuery` | 协议包保留但节点只回一条"不再保留运行记录"，避免旧 cli 等 10s 超时 |

上下文阀（32KB 只回末尾）的提示行也改了：不再指路 `terminal_query`，改成
"自己 `| tail -50` 或重定向到文件再 read_file"（终端不再替你留全量日志）。

`test_terminal_tools` 同步重写（原来一半用例在测 query/run_id）：现在覆盖
前台回包、错误分支、`&` 后台、单前台互斥、cancel、上下文阀、全新终端 —— **ALL PASS**。
