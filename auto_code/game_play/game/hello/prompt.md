# hello 玩法 —— 交一份能通关的 `play/play.ts`

容器里就两个进程：**`hello`**（题目侧的游戏，不许改）+ **`script`**（你要写的脚本 = `play/play.ts`）。
两者只通过"同一块屏幕 + 键鼠"发生关系 —— 脚本**看不见**游戏的任何内部状态。

## 规则（画面上能看到什么）

- 窗口 `HELLO`，客户区 **480x360**（无头下 scale=1 ⇒ 截图尺寸就是 480x360）；浅灰底 `#f2f2f2`，顶部 64px 是 HUD（分数那行）。
- 场上 3 个球：**红 `#e02020`** / **蓝 `#2050e0`**，半径 34 的纯色圆 + 2px 黑边；每个球每 4 秒换个位置重生（画面一直在动，别背位置）。
- 鼠标点在球**圆内** = 命中；**点红 +1 分、点蓝 -1 分**。
- 初始 **5 分**：**到 10 分通关**（画面出现绿色 `CLEAR!` `#1a8f3c`），**掉到 0 分输**（`GAME OVER`）。
- 键盘只有一个作用：`R` / 回车 = 重开（输了没必要重开 —— 这局就没过）。

## 判分（这个玩法自己的尺子）

`game/hello/verify.py` 在容器**外面**看同一块屏，并读游戏自己写的状态文件，给出：

| 结论 | 含义 |
| --- | --- |
| `cleared` | 通关（`score` 到 10）—— **尺子退出码 0 = 这局算过** |
| `game_over` | 掉到 0 分 |
| `running` | 到点还在跑（没通关也没死） |
| `program_gone` / `script_gone` | 进程没了 |

⚠ 那个状态文件是**尺子的特权**：脚本**不许读**它，也不许读 `game/**` 里的任何文件（读了就是开卷，尺子也就成了空指标）。

## 脚本能用什么（就这些）

`__jz.computeruse.*`（类型声明在 `play/jzapi.d.ts`，语义与 C++ 一致）：

| 成员 | 说明 |
| --- | --- |
| `screenshot()` | 整块桌面的 PNG data URL ——"看"的唯一入口 |
| `click(x, y, btn)` / `doubleClick` / `drag(...)` | **物理像素**坐标（相对整块桌面 = 截图像素；屏尺寸问 `screenInfo()`）；**三个参数都不能省**（`btn` 给 `'left'/'right'/'middle'`）；命中窗口会激活它 |
| `key(name)` | 按一下键（**Win32 键名**：`Space` / `Return` / `R` / `Left` …） |
| `type(text)` | 打字；`wait(ms)` 阻塞等（在脚本自己的线程上睡，游戏照跑） |
| `layoutTree(json)` | 目标窗口的可访问布局；找不到窗口返回 null（可当"窗口在不在"用） |
| `match(json)` | 像素/文字兜底：`{"kind":"color","color":"#rrggbb"}` / `{"kind":"image",...}` / `{"kind":"ocr","text":...}`；找不到返回 null（**不假装成功**） |
| `ocrReady()` / `ocrTexts()` | 本地 OCR（没接后端一律"未找到"） |

另外：`fs` / `buffer`（写日志与截图）、`setTimeout/setInterval`、`console`。

- 所有坐标（`screenshot` / `match` 的框 / `click`）基准**都是整块桌面**，可直接互用：`match` 给的框中心就能当 `click` 的入参。
- ⚠ 别用 `path` 模块拼 Windows 绝对路径（容器里的 `path` 是 POSIX 口径，会给成 `/E:/...`）；自己用 `/` 拼。

## 参数从哪来

运行根（= 你的 js 所在目录 `build/play/`）下的 `play.json`，由驱动写：

```json
{ "out": "<产物目录绝对路径>", "app": "hello", "play": "hello", "window": "HELLO",
  "warmupSec": 1.5, "steps": 120, "stepMs": 300, "maxSeconds": 0 }
```

按 `steps` / `maxSeconds` 收工（0 = 不看）。

## 要交什么

1. **`play/play.ts`**（唯一产出；编译由驱动做，你只管把 TS 写对 —— 编译不过驱动会告诉你）；
2. 收工时写 `<out>/session.json`（`reason` / `steps` / `clicks` / `frames` / `errors`）—— 没写 = 脚本卡死；
3. 隔几拍把 `cu.screenshot()` 存到 `<out>/frames/`（给人、给下一轮的你看画面）。

## 红线

- ✅ 只**看画面**（`screenshot` / `match`）、只**操作鼠标键盘**（`click` / `key` / `type` / `wait`）。
- ⛔ 不读游戏内部状态（状态文件、控件树、模拟器内存），不往游戏里塞代码。
- ⛔ 不改 `game/**`、`verify.py`、`task.json`、`baseline.json`。
- ✅ 随便改 `auto_code/game_play/play/**`（你自己写的东西）。

## 证据在哪（下一轮开头会给你）

`runs/current/`：`play.log`（脚本每拍一行，自己写的）、`frames/`（脚本自留）、`external/` + `metrics.jsonl`（驱动从**外面**抓的帧与帧间差异）、`report.md` + `result.json`（尺子的一页结论）。
