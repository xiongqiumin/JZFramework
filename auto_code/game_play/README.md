# game_play —— 交一个"一键通关脚本"（驱动说明，**给人看**）

给 agent 的任务书**不在本文件**：它是**玩法的一部分**，住在玩法目录里 —— `game/<玩法>/prompt.md`
（一个玩法一份，跟本体 / `verify.py` / `config.json` 同级；换玩法就换任务书）。

## 两个目录（**题目只读、工作区可写**）

一次运行 = **一份 `run.json`** 说了算，里面最要紧的就是这两个目录：

| | 是什么 | 谁写 |
| --- | --- | --- |
| `testDir` | **题目（玩法）目录**：`prompt.md` 任务书 / `config.json` 怎么跑怎么判 / `verify.py` 尺子 / 本体 `.ts`（+ 可选参考脚本 `play.ts`） | 只读（人写） |
| `workDir` | **这一次运行的工作区**：产出、编译产物、agent 配置、证据、归档 | 全是驱动/agent 写 |

```
auto_code/game_play/                      # 任务目录（驱动代码与题目；尽量干净）
├─ README.md          # 本文件（人看）
├─ run.json           # **唯一输入**：一次运行的配置（题目 + 工作区 + 与题目无关的调参）
├─ run.py             # 驱动：起容器 + 起 agent_cli + 逐轮【设计→编译→跑局→判分→分析】
├─ env.py             # 环境侧：容器/agent 生命周期 + play.json / game.json 写入口 + 像素工具
├─ run_veirfy.py      # 只跑一局 + 尺子（调脚本用；同样只吃 run.json）
├─ tsconfig.json      # 编译口径的单一来源（工作区那两份 tsconfig 由它派生）
├─ jzapi.d.ts         # 脚本能用的面（模板：驱动每次拷进工作区 play/）
└─ game/<玩法>/        # 题目：prompt.md + 本体 + verify.py + config.json（+ watch.py 可选）

<workDir>/                                # 工作区（示例：build/auto_code/game_play/hello）
├─ play/play.ts        # **产出**（agent 改的就是它；首次按 seed 铺：默认用题目自带参考脚本，没有就空骨架）
├─ play/jzapi.d.ts     # 脚本能用的面（从任务根模板拷）
├─ tsconfig.json       # 编产出 → build/play/play.js
├─ tsconfig.game.json  # 编题目本体 → build/game/<本体>.js（rootDir=题目目录 ⇒ 产物路径可预期）
├─ agent_cli.json      # agent 配置（llm / terminal.rootDir=工作区 / 题目目录进 trustedDirs / logFile）
├─ plan.json           # 这次"会怎么做"（路径、命令、agent 设置）——`--dry-run` 主要给你看这个
├─ build/              # tsc 产物（容器按它装 game + play）
├─ runs/current/       # 本局证据（每轮被覆盖）；runs/round_NNN/ 归档；runs/scores.jsonl 一行一轮
└─ Agent.log           # agent 回合诊断（think / tool / reply）
```

容器 `virtual_device --headless --desktop <玩法桌面>` 里只有两个进程：`<玩法程序名>`（题目侧游戏）与 `script`（产出脚本），
只通过屏幕 + 键鼠发生关系。

## 怎么跑

```powershell
python auto_code\game_play\run.py                      # 读任务目录的 run.json（不给路径就用它）
python auto_code\game_play\run.py my_run.json          # 换一份运行配置（一次运行 = 一份）
python auto_code\game_play\run.py --dry-run            # 只准备 + 编译 + 打印计划/结构，不碰容器与 agent
python auto_code\game_play\run.py --no-agent           # 不起 agent，只跑局（调链路 / 复验脚本）

python auto_code\game_play\run_veirfy.py <run.json> <play.ts>   # 只跑一局 + 尺子（复验某一份脚本）
python auto_code\game_play\run_veirfy.py <run.json>            # 不给脚本 = 复验工作区里的产出

python auto_code\game_play\game\hello\watch.py --watch                    # 旁路看：轮次/画面/分数/日志尾
python auto_code\game_play\game\hello\watch.py --ask-file ask.txt --with-screen   # 给 agent 下一道你的活（可带画面）
```

- **相对路径一律按仓库根解析**（不是 CWD）⇒ 同一份 `run.json` 在哪儿调用都指向同一处。
- 产出的"起点"**一律是空的**：工作区首次生成 `play/play.ts` 时只写一份空骨架（入口脚手架 + TODO，
  不给任何现成解法——"能不能做出来"是这道题的全部）；之后**不覆盖**它（想重来就删掉它，或换一个 `workDir`）。

## 驱动每一轮做什么（`run.py`）

```
① 让 agent 设计/改脚本   POST /task（环境简报 + **题目任务书原文**；不给 verify_cmd，验证由驱动做）
② 编译                   tsc：工作区(play) + 题目本体（都出到工作区 build/）；不过就把错误带进④
③ 跑一局                 卸旧程序 → 装 game + play → 外面抓帧 → 等 session.json
④ 判分 + 归档            题目自己的 verify.py（子进程）→ runs/round_NNN
⑤ 让 agent 看证据自己决定 POST /task（它自己读 play.log / report.md / metrics.jsonl / 尺子输出）
⑥ 出口                   轮次上限 / 总时长 / 连续 stallRounds 轮没进展 / agent 回 [script-ok] / stopWhen=verify-pass
```

- agent 是主体：它自己观测、自己改工作区里的产出；驱动只负责"拉起来、别让它空转、跑局判分、归档"。
- ⚠ `POST /task` 是**连接保持**式的：驱动侧超时断开**不会**杀 agent 手里的任务 ⇒ 设计轮回包丢了要先
  等它空闲（`AgentCliProc.wait_idle`，上限 `agentDrainSec`）再编译，否则就是拿一个还在变的文件做实验。

## `run.json` 字段（除两个目录外，都是**与题目无关**的调参；缺哪项用内置默认）

| 键 | 含义 |
| --- | --- |
| `testDir` / `workDir` | 题目目录 / 工作区（相对仓库根或绝对） |
| `cfg` | 构建配置（找 `virtual_device.exe` / `tsc`；`Debug`/`Release`），也可用 `tsc.path` 直接指 tsc |
| `llm` | agent 要打的多模态端点（`url`/`name`/`token`/`netTimeoutMs`）；**不写就用 agent_cli 自己那份 config.json 的** |
| `agent` | `enabled` / `bin` / `readySec` / `required` / `restart` |
| `rounds` | `max` / `timeoutSec`（单轮 /task 上限）/ `stallRounds` / `retrySec` |
| `stopWhen` | `verify-pass`（默认：尺子 PASS 就收工）/ `rounds`（跑满） |
| `build` / `archive` | 是否编译 / 是否归档到 `runs/round_NNN` |
| `agentDrainSec` | 设计轮回包超时后，等 agent 收尾的上限（默认 1800） |
| `steps` / `stepMs` / `warmupSec` / `seconds` / `target` | 节奏覆盖（不写就按题目 `config.json`） |

⚠ **两处环境前提**（换机器先看）：① `virtual_device.exe` 必须与源码同版本
（`cmake --build build --config Debug --target virtual_device`；旧 exe 会让容器里的 `cu.screenshot()` 打到**真桌面**）；
② llm 要指**活着的**端点（本机 `http://192.168.111.1:8080/v1/chat/completions`，模型 `my_model`）。

## 产物 / 归档 / 怎么看

- 全在**工作区**：`runs/current/`（本局，每轮覆盖）＝ `play.log` + `frames/`（脚本自留）、
  `external/` + `metrics.jsonl`（驱动从外面抓的帧与差异）、`session.json`（脚本自报）、
  `report.md` + `result.json`（尺子结论）、`Agent.log`（agent 回合诊断）、`agent_cli_stdout.txt` / `container_stdout.txt`；
  `runs/round_NNN/`（每轮归档，上面这些 + 抽样的帧 + 状态文件抄本）、`runs/scores.jsonl`（一行一轮，`pass` 取自尺子退出码）。
- 判分失败 → 看尺子打印的结论（`cleared`/`game_over`/`running`…）；脚本报错 → `play.log` 的 `!!` 行 + `session.json.errors`；
  游戏本身 → `container_stdout.txt`；玩得好不好 → **只看画面**（`report.md` 内嵌的关键帧，或 `frames/` / `external/` 任意一张）。

## 已知边界（都还没做，别当成 bug）

1. **状态文件仍锚在任务根**：`game/<玩法>/verify.py` 自己按题目 `config.json` 的 `statusPath` 算
   （`<任务根>/runs/current/...`），驱动只能对齐它并把结果抄一份进工作区。要收进工作区得给尺子加 `--status-path`。
   ⇒ 用**同一个玩法**并发跑两份工作区会撞这个文件（顺序跑没事）。
2. `run_veirfy.py` 的复验脚本挪到 `<work>/verify/`（**不动**工作区里的产出），证据落 `<work>/runs/verify/`；
   编译产物 `<work>/build/verify/play/play.js`。
3. 任务级 `tsconfig.json` 仍是编译口径的来源（工作区那两份 tsconfig 由它派生）。
   （`task.json` 已删：驱动不读它；尺子走 `env.play_config()` 时也会**回落**到 `game/<玩法>/config.json`。）
