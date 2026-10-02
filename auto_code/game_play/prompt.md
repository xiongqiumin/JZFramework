# 任务：交一个"一键通关脚本"，把一个游戏打通关

## 最终产出是什么

**一件东西：`play/play.ts` —— 跑在 virtual_device 容器里的自动化脚本。**

它是一条命令跑完的那种脚本（形状如下），由**驱动**装进容器执行：

```ts
// play/play.ts（节选；完整版见文件本身）
active_window(shooter);      // 点一下把游戏窗口激活（CU 的单击会命中并激活窗口）
while (!玩完) {
    screentCaputer();        // 截图（同时落盘 = 给外部观测的"界面"）
    dosomeThing();           // 决策 → key()
}
```

```powershell
cd auto_code\game_play
python run.py                              # **一个配置文件说了算**（默认读本目录 task.json）
python run.py --config build\run_gba.json  # 换一份配置（玩法/拍数/是否归档…都在配置里）
```

运行时**大模型不参与**：脚本自己按逻辑跑完一局。大模型在**局与局之间**干活：
看 `play.log` / `frames/`（脚本自己留的）与 `external/`（驱动从外面抓的帧），然后回来改脚本。

## 玩法（**自包含目录**；`task.json` 的 `plays` 只登记它的 `dir`）

每个玩法是一个目录，自带三件东西：

- `game.ts` —— **题目侧游戏**（不许改）
- `verify.py` —— **这个玩法自己的尺子**：清容器 → 装 game → 装 play → 观察 → 判定
- `config.json` —— 怎么跑 / 怎么判：进程名、窗口、桌面、原生尺寸、拍数、`targetScore`（通关线）

| 玩法 | 目录 / 入口 | 窗口 / 客户区 | 容器桌面 | 要 ROM | 通关口径 |
| --- | --- | --- | --- | --- | --- |
| `shooter`（默认） | `game/shooter/`（`game.ts`） | `SHOOTER` 800x600 | 800x600 | 否 | 打死敌人计分（+10），**到 `targetScore`(=300) 分算通关**：`game.ts` 判定、画面显示 `CLEAR!` |
| `gba` | `game/gba.ts`（暂未目录化） | `GBA` 240x160 | 320x240 | 是 | 没有自动判据（整部通关太难，代码保留） |

- 尺子**归玩法所有**：它自己清掉容器里的程序、装 game + play、从外面抓帧、读**状态文件**
  （`score/lives/over/win` —— 游戏自己每 500ms 写的），给出 `cleared / game_over / running …`；
  **退出码 0 = 通关**。顶层 `run.py` 只负责把它跑起来、拿退出码、归档。
- ⚠ 状态文件是**尺子的特权**：脚本按红线**不许读**它（读了就等于开卷查答案）。
- 桌面/原生尺寸/窗口标题/进程名都在玩法目录的 `config.json` 里；顶层配置的同名字段可覆盖。

- 玩法用 `run.py --app <名>` 选（默认取 `task.json` 的 `defaultPlay`，现在是 `shooter`）；
- 每个玩法的入口 ts / 进程名 / 窗口标题 / 桌面尺寸 / **原生尺寸** / 要不要 ROM 全在 `task.json` 的 `plays` 里；
- 驱动会把 `play`（玩法名）与 `app`（进程名）写进 `build/play/play.json`，脚本据此选**键位**与**策略**。

## 你要走的循环（这就是这个任务的形状）

| 步 | 干什么 | 命令 / 看什么 |
| --- | --- | --- |
| 1 探索 | 摸清这台 App：按哪个键、画面怎么变 | `python run.py --steps 40` 跑一局，看 `external/` 的帧序 + `report.md` |
| 2 设计脚本 | 把"看到什么 → 按什么"写进 `play/play.ts` | 主战场；能用的面只有 `__jz.computeruse.*` + `fs`（见下） |
| 3 运行脚本 | 让它在容器里跑 | `python run.py` |
| 4 观测 | 看日志 + 看界面 | `runs/current/play.log`（脚本每拍一行）、`frames/`（脚本自己截的）、`external/` + `metrics.jsonl`（**外面**抓的）、`report.md`（一页摘要 + 关键帧） |
| 5 调整 | 按观测到的**一处**问题改脚本，再跑 | 例：ship 站着不动被打死 ⇒ 先让"敌机在哪"变成脚本能算的（像素/颜色/位置），再改决策 |
| 6 产出 | 反复 1~5 直到脚本真能把这个游戏打下去 | 归档在 `runs/round_NNN/`；"通没通关"由人看画面定 |

## 现状（2026-10-02 实测，动手前先看清楚）

链路全通，**两种玩法各跑通一局**（`shooter` → `runs/round_025/`；`gba` → `runs/round_026/`）：

```
容器 virtual_device --headless（`--desktop <玩法桌面>`，无头下 scale 恒 1）
  │  —— 容器里就**两个进程**，互相只见得到"屏幕 + 键盘鼠标"：
  ├─ 进程 "shooter"（game/shooter.ts，**题目侧**）：800x600 窗口 + 自绘游戏控件（飞行射击）
  └─ 进程 "script"（play/play.ts，**产出物**）：用 __jz.computeruse.* 操作这块桌面
       · 看：screenshot() → 整块桌面的 PNG data URL（脚本自己存盘 = frames/）
       · 操作：click(x, y, btn)（归一化坐标，命中窗口会激活它）/ key('Left') / wait(ms)
       · ⚠ key() 是"按下+松开一瞬间"：要**按住**就按帧重复调用（见脚本里的 hold()）
```

已确证的事实：

- 脚本**真的在操作这个游戏**：`round_025` 里脚本按了 40 次键（`Left`/`Right`/`Space`），
  外部帧序列**显著变化 8 次**（最大差异 99.58%）—— 游戏在跑、在响应；
- 容器里的 CU 后端 = 这块虚拟桌面（`main.cpp` 里 `setVirtualDesktop(&m_screen)`）——
  所以脚本写起来跟真桌面上的自动化脚本一模一样；
- 容器侧适配（`game/shooter.ts`，只动了三处、不改玩法）：① 窗口标题 → `SHOOTER`；
  ②③ 两处定时器句柄的**类型注解**（容器 tsconfig 的 `setInterval` 返回 Node 的 `Timeout`）。
  另外 `FixedTimer` 的**落后保护阈值 5 → 30**：容器里 JS `setInterval` 精度远低于 C++ 的
  `JZFixedTimer`，启动瞬间就落后 11 帧 ⇒ 阈值 5 会让游戏开跑几秒内自己停机（画面定格）。
- **当前骨架策略还不会玩**：`shooterStrategy` 只会"左右游走 + 一直开火"，不看敌机子弹 ——
  这正是要改的地方（先有"看懂画面"的手段，再谈决策）。

## 允许改哪里

- ✅ `auto_code/game_play/play/**`（**产出物本体**：脚本与它自己的模块，随便改）
- ✅ `JZJSEngine/js_wrapper/**`、`JZWidget/`、`JZEmulator/`、`binary/virtual_device/`、`JZComputerUse/`
  （确实缺底层能力时 —— ⚠ 这些在模块里，改完**必须重编**：
  `cmake --build build --config Debug --target virtual_device`）

## 禁止改哪里（红线）

- ⛔ `auto_code/game_play/game/**`（**题目侧**：游戏怎么开、窗口多大、怎么喂键 ——
  改了就不是同一道题；`shooter.ts` 是从 `test/widget/shooter`（C++）逐项对齐移植来的）
- ⛔ `auto_code/game_play/verify.py`、`baseline.json`（**尺子**：不许为了让结果好看而改口径）
- ⛔ **不许读游戏内部状态**：模拟器内存/存档、控件树、`/api/programs/<n>/eval` 之类内部真相。
  脚本能"看"的只有画面（`screenshot()`），能"做"的只有鼠标键盘。一旦读了内部状态，
  这就不是"视觉 agent"了，尺子也成了空指标。
- ⛔ 不许把"第几拍按什么"写死成一张表糊弄 —— 游戏状态随游戏走，不随你走。

## 脚本能用什么（就这些）

`__jz.computeruse.*`（类型声明在 `play/jzapi.d.ts`，语义与 C++ 一致）：

| 成员 | 说明 |
| --- | --- |
| `screenshot()` | **整块虚拟桌面**的 PNG data URL ——"看"的唯一入口 |
| `click(x, y, btn)` / `doubleClick` / `drag(...)` | 归一化坐标（0~1）的鼠标操作；**命中窗口会激活它** |
| `key(name)` | 按一下键（**Win32 键名**：`Space`/`Return`/`R`/`Left`/`Right`/`Up`/`Down`/`A`…，组合写 `Ctrl+C`）。⚠ 三个参数不能省：`click` 必须给 `btn` |
| `wait(ms)` | 阻塞等（在脚本自己的线程上睡，游戏在另一个线程照跑） |
| `layoutTree(json)` | 目标窗口的可访问布局；找不到窗口返回 null（可当"窗口在不在"用） |
| `match(json)` / `ocrTexts()` | 像素/文字兜底（模板匹配要 OpenCV、OCR 要模型；没接后端一律"未找到"） |

另外：`fs` / `path` / `buffer`（写日志与截图；⚠ 别用 `path.join` 拼 Windows 绝对路径，
容器里的 path 是 POSIX 口径，会给成 `/E:/...`）、`setTimeout/setInterval`、`console`。

**键名怎么给**（脚本是"拿着键盘的那个人"）：

- `shooter` 玩法：键名与游戏一致 —— `Left`/`Right`/`Up`/`Down`（或 `A`/`D`/`W`/`S`）、`Space` 射击、`R` / `Return` 重开；
- `gba` 玩法：游戏机上印的是 `A`/`B`/`Start`/...，要翻成键盘上的 `Z`/`X`/`Return`/... ——
  权威映射在 `JZEmulatorView::gbaKeyOf`，脚本里是 `GBA_KEY` 那张表，两边必须一致，否则白按。

## 参数从哪来

**驱动的命令行只剩 `--config`**（默认 `task.json`）—— 拍数/预热/玩法/是否编译/是否归档，
以及"拉不拉 agent_cli"，全在配置的 `run` 段里：

```json
"run": { "play": "", "cfg": "Debug", "build": true, "archive": true, "desktop": "",
         "steps": 120, "stepMs": 400, "warmupSec": 3, "seconds": 0, "timeoutSec": 0,
         "agent": { "enabled": true, "bin": ".../agent_cli.exe", "config": "build/agent_cli.json",
                    "httpUrl": "", "token": "", "readySec": 120, "required": true },
         "verify": "verify.py" }
```

驱动每轮还写 `build/play/play.json`（**脚本**按运行根找它）：

```json
{ "out": "<绝对路径>", "app": "shooter", "play": "shooter", "window": "SHOOTER",
  "warmupSec": 3, "steps": 120, "stepMs": 400, "maxSeconds": 0 }
```

脚本**必须**在这几件事上配合：按 `steps`/`maxSeconds` 收工，并把自报结果写进 `<out>/session.json`
（`reason`/`steps`/`keys`/`frames`/`errors`）—— 尺子要读它（见下）。

## 怎么跑

```powershell
cd auto_code\game_play

python run.py                        # 读 task.json（唯一配置）：起容器 → 起 agent_cli → 进入循环
python run.py --config build\run_gba.json   # 换配置文件（示例：gba 玩法、20 拍、只跑不归档）
python run.py --config build\run_fast.json  # 快跑（2 轮/8 拍），用来验循环骨架

python checks/chain_smoke.py         # 链路自检（gba 玩法：容器/截图/按键那条**外部**通路还通不通）
python verify.py --self-test         # 只校尺子
python verify.py --dir runs/round_028   # 复验某一轮归档
```

**驱动每一轮做的事**（`run` 段控制，见 §参数从哪来）：

```
① 让 agent 设计/修改脚本   POST /task（含任务书 + 证据目录路径；**不给 verify_cmd**）
② 编译                     tsc（agent 改的是 play.ts，容器装的是 play.js）；编译不过就把错误带进④
③ 跑一局                   卸旧程序 → 装 game + 装 play → 外面抓帧 → 等 session.json
④ 判分 + 归档              verify.py（子进程）→ runs/round_NNN
⑤ 让 agent 看证据自己决定  POST /task（它自己读 play.log / report.md / external/…，自己决定改不改）
⑥ 出口                     轮次上限 / 总时长上限 / 连续 stallRounds 轮没进展 / agent 回 [script-ok]
```

⚠ **agent 是主体**：它自己观测（自己读 `runs/current/` 的证据，自己决定要不要截图/抓日志）、
自己改 `play/play.ts`；驱动只负责"把它拉起来、让它一直转、跑局判分、别让它无限空转"。
`POST /task` 是**阻塞**的（一轮干完才回包），所以"agent 还在监控时不提交"这件事天然成立。

产物：`runs/current/`（本局：`play.log`/`frames/` 脚本留的，`external/`+`metrics.jsonl` 驱动抓的，
`session.json` 脚本自报，两侧 stdout 日志，`report.md` 摘要）、`runs/round_NNN/`（每轮归档）、
`runs/scores.jsonl`（一行一轮）。`build/` 与 `runs/` 都不入库。

## 判定（重要：这一版**不判通关**）

`verify.py` 用**两路证据互相印证**，判七道机械硬门：

| 门 | 判什么 |
| --- | --- |
| `container_ok` | 容器起来了、游戏窗口在了（拿到客户区几何） |
| `window_native` | 客户区 = **该玩法的原生尺寸**（shooter 800x600 / gba 240x160；期望值来自 `drive.json` 的 `native`，由驱动按 `task.json` 的 `plays` 填，缺省回落 240x160） |
| `script_ran` | 脚本自己写了 `session.json`（= 它收工了，没卡死） |
| `script_acted` | 脚本自报**按过键**（它在操作，不是干看着） |
| `external_alive` | 外面抓的帧够数、尺寸都对（全程有人盯着这块屏） |
| `screen_alive` | 外部帧里至少一帧不是单色块（真画面，不是白屏卡住） |
| `screen_changed` | 外部帧序列里至少一次显著变化（画面在动 = 游戏在响应） |

全过 ⇒ `PASS`（退出码 0），**故意不打 `[loop-stop]`**：跑起来 ≠ 通关，收工由人看画面定。

要**自动**判通关，两条路（都还没做，别自己偷偷开洞）：
① 给**尺子**开一个读游戏内部状态的口子（GBA 玩法可改 `E:/work/simulator` 那侧的 `mgba_host.dll`
加 `savestate/read_memory`；shooter 玩法则要给它加一个"分数/结束"的只读出口），
真值从 RAM/游戏态读 —— 那是**尺子**的特权，脚本永远不许用；
② 定"画面语义"口径（比如 shooter 的 `GAME STOPPED` 遮罩、GBA 某章的标志画面做模板匹配）。
两条都要先改 `verify.py` + `baseline.json`。

## 失败信息怎么用

- 判分失败 → 打印没过的是哪几道门（`script_ran`/`external_alive`/`screen_alive`…），照它改一类，别同时改五处；
- 脚本里的错 → `runs/current/play.log` 的 `!!` 行 + `session.json` 的 `errors` + `play_stdout.txt`；
- 游戏机本身的问题 → `container_stdout.txt`（容器侧日志，游戏自己的 `console.log/warn` 也在里面）+ `drive.json`；
- 玩得好不好 → **只看画面**：`report.md` 里内嵌的外部关键帧，或 `external/`、`frames/` 里的任意一张。

## §探索（"app 探索"这一步怎么落）

没有单独的 explore 模式了 —— 脚本是**在容器里跑的程序**，探索就是"跑一局、把看到的一切留下来"：

- 把 `--steps` 调大、`--step-ms` 调小（例如 `python run.py --steps 200 --step-ms 300`），
  在脚本里对每个键**逐个试按**并记日志；
- 看得见的素材：`frames/`（脚本视角，含每拍的图）、`external/`（外面视角）、`metrics.jsonl`
  （每张外部帧的颜色数/非黑/与上一帧的差异 —— 用来分辨"画面自己在动"还是"按键造成的"）；
- 把这些素材看完，再决定脚本里的判断该怎么写。

## §存档（只有 gba 玩法有这个问题）

`gba` 玩法：`JZEmulator` 只暴露 `openRom/start/stop/frameCount`，没有读/写存档的接口
⇒ 每局都从开机 logo 开始，要"从第 3 章继续调"得先给容器/模拟器加存档能力（框架的活）。

`shooter` 玩法：没有存档这个概念，每局都是满血 3 条命从头开始 —— 这正是它比 GBA 好练的地方。
