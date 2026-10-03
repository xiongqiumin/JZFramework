# shooter 玩法 —— 交一份能把这个飞行射击打下去的 `play/play.ts`

容器里就两个进程：**`shooter`**（题目侧的游戏，不许改）+ **`script`**（你要写的脚本 = `play/play.ts`）。
两者只通过"同一块屏幕 + 键鼠"发生关系 —— 脚本**看不见**游戏的任何内部状态。

## 规则（画面上能看到什么）

- 窗口标题 `JZWidget GDI - 飞行射击`，客户区 **800x600**（无头下 scale=1 ⇒ 截图尺寸就是 800x600）。
- 自绘的飞行射击：自己的飞船在下方，敌机从上方来，会被打、也会撞 —— 满血 3 条命，扣光就 `GAME OVER`。
- **一架敌机 +10 分**（大机小机一样）；**越打越难**：刷怪间隔 `max(10, 46 - score/40)` 帧、
  敌机下移 `2 + score/160` —— 分数越高敌人越密、越快。
- 敌机有 **22% 概率掉道具**（18x18 的方块：`P` 多发 / `R` 环射 / `+` **回血 +1 命，最多 5 命**）
  ⇒ 想打到 300 分（30 架），多半得靠捡 `+` 续命。
- 键盘（与游戏一致）：`Left` / `Right` / `Up` / `Down`（也认 `A` / `D` / `W` / `S`）移动，`Space` 射击，`R` / 回车 重开。
  ⚠ `key()` 是"按下 + 立刻松开"的一瞬间：要**按住**（连续移动）得按帧重复调用。
- ⚠ 这个玩法**没有存档**：每局都是满血 3 条命从头开始。

## 判分（这个玩法自己的尺子）

**通关线 = 300 分**（`config.json` 的 `targetScore`；一架 +10 分 ⇒ **30 架**）。
⚠ 死了按 `R` 重开会**清分**（`reset()` 里 `score = 0`）⇒ 必须**一局之内**打到 300，中途死掉从头来。

本体只往容器 stdout 打印状态行，尺子读**容器日志**判：

| 结论 | 含义 |
| --- | --- |
| `cleared` | 打到 300 分 —— **通关**（退出码 0） |
| `running` | 跑起来了 / 到点还在跑（没通关） |
| `game_over` | 打到死（没通关；也是有效的一局） |

"打得好不好"也可以看画面（`runs/current/report.md` 里内嵌的外部关键帧）。
⚠ 那几行打印（`[shooter] score=..` / `alive ..` / `CLEAR score=..` / `GAME OVER score=..`）
是**尺子的特权**：脚本**不许读**它们（脚本只能看画面）。

## 脚本能用什么（就这些）

`__jz.computeruse.*`（类型声明在 `play/jzapi.d.ts`，语义与 C++ 一致）：

| 成员 | 说明 |
| --- | --- |
| `screenshot()` | 整块桌面的 PNG data URL ——"看"的唯一入口 |
| `click(x, y, btn)` / `doubleClick` / `drag(...)` | **物理像素**坐标（相对整块桌面 = 截图像素；屏尺寸问 `screenInfo()`）；**三个参数都不能省**（`btn` 给 `'left'/'right'/'middle'`）；命中窗口会激活它 |
| `key(name)` | 按一下键（**Win32 键名**：`Space` / `Left` / `Right` / `R` …） |
| `type(text)` | 打字；`wait(ms)` 阻塞等（在脚本自己的线程上睡，游戏照跑） |
| `layoutTree(json)` | 目标窗口的可访问布局；找不到窗口返回 null（可当"窗口在不在"用） |
| `match(json)` | 像素/文字兜底：`{"kind":"color","color":"#rrggbb"}` / `{"kind":"image",...}` / `{"kind":"ocr","text":...}`；找不到返回 null（**不假装成功**） |

另外：`fs` / `buffer`（写日志与截图）、`setTimeout/setInterval`、`console`。

- 所有坐标（`screenshot` / `match` 的框 / `click`）基准**都是整块桌面**，可直接互用。
- ⚠ 别用 `path` 模块拼 Windows 绝对路径（容器里的 `path` 是 POSIX 口径）；自己用 `/` 拼。

## 参数从哪来

运行根（= 你的 js 所在目录 `build/play/`）下的 `play.json`，由驱动写：

```json
{ "out": "<产物目录绝对路径>", "app": "shooter", "play": "shooter",
  "window": "JZWidget GDI - 飞行射击", "warmupSec": 2.0, "steps": 120, "stepMs": 400, "maxSeconds": 0 }
```

按 `steps` / `maxSeconds` 收工（0 = 不看）。

## 要交什么

1. **`play/play.ts`**（唯一产出；编译由驱动做，你只管把 TS 写对）；
2. 收工时写 `<out>/session.json`（`reason` / `steps` / `keys` / `frames` / `errors`）—— 没写 = 脚本卡死；
3. 隔几拍把 `cu.screenshot()` 存到 `<out>/frames/`（给观测用）。

## 红线

- ✅ 只**看画面**、只**操作鼠标键盘**。
- ⛔ 不读游戏内部状态（那几行 stdout 打印、控件树、往游戏里塞代码）。
- ⛔ 不改 `game/**`（含 `binary/code_agent/runtime/test_jsshooter/**`）、`verify.py`、`task.json`、`baseline.json`。
- ✅ 随便改 `auto_code/game_play/play/**`（你自己写的东西）。

## 证据在哪（下一轮开头会给你）

`runs/current/`：`play.log`（脚本每拍一行）、`frames/`（脚本自留）、`external/` + `metrics.jsonl`（驱动从**外面**抓的帧与帧间差异）、`report.md` + `result.json`（尺子的一页结论）、`container_stdout.txt`（游戏自己打印的状态行也在这）。
