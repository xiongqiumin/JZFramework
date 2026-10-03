# gomoku 玩法 —— 交一份能**在 3 分钟内下赢电脑**的 `play/play.ts`

容器里就两个进程：**`gomoku`**（题目侧的五子棋，不许改）+ **`script`**（你要写的脚本 = `play/play.ts`）。
两者只通过"同一块屏幕 + 键鼠"发生关系 —— 脚本**看不见**游戏的任何内部状态，只能看画面。

## 规则（画面上能看到什么）

- 窗口 `五子棋`，客户区 **600x676**（无头下 scale=1 ⇒ 截图尺寸就是 600x676）。从上到下三块：
  | 区域 | 位置 | 内容 |
  | --- | --- | --- |
  | 状态栏 | y ∈ [0, 36) | 一行字：`轮到你 (黑棋, 点击落子)` / `电脑思考中...` / `你赢了!` / `电脑赢了!` / `平局` |
  | 棋盘画布 | y ∈ [36, 636)，x ∈ [0, 600) | 15x15 棋盘（自绘） |
  | 按钮 | y ∈ [636, 676) | `重新开始`（点它会清盘重来 —— 别点它） |
- **你执黑，先走**；电脑执白，**你落一子它立刻回一手**（同一瞬间，画面直接变成两子）。
- 棋盘：15x15 交叉点，**格距 40px**，棋盘四周留白 **20px**。交点 `(r, c)`（r=行 0~14 从上往下，
  c=列 0~14 从左往右）在**棋盘画布内**的位置是 `(20 + 40*c, 20 + 40*r)`；画布本身在客户区里 y 偏移
  36（状态栏高）。
- ⚠ **别硬套像素、更别假设"桌面原点 = 客户区原点"**：CU 的帧就是客户区 **600x676**，`match` 的框与
  `click` 的入参是**同一个坐标系**（都相对整块桌面）—— 正路是：先 `match({kind:'color',color:'#e3b572'})`
  拿到棋盘矩形，再按 `(20+40c)/600`、`(20+40r)/600` 的比例算交点（实测这样"点哪落哪"；硬编码像素
  在边缘格上会落错）。
- 颜色：棋盘底 `#e3b572`、网格线/星位 `#3a2a12`、**黑子 `#111111`（你）**、白子 `#f6f6f6`（电脑），棋子半径 17px。
- 胜负：横 / 竖 / 斜任意方向**先连成 5 子**者胜；棋盘下满无胜者 = 平局。

## 判分（这个玩法自己的尺子）

`game/gomoku/verify.py` 在容器**外面**看同一块屏，并读**容器日志里本体打印的事件行**（形如
`[vproc: "gomoku"] "[gomoku] ai move r=7 c=8"`）：

| 本体打印 | 含义 |
| --- | --- |
| `[gomoku] start grid=15 cell=40 board=600x600 …` | 这局开张了（窗口起来了） |
| `[gomoku] human move r=.. c=..` | 你（黑）落在 (r,c) |
| `[gomoku] ai move r=.. c=..` | 电脑（白）落在 (r,c) |
| `[gomoku] RESULT=human_win …` | **玩家赢** ⇒ 尺子判 `cleared`（**退出码 0 = 这局算过**） |
| `[gomoku] RESULT=ai_win …` | 电脑赢 ⇒ `game_over` |
| `[gomoku] RESULT=draw …` | 平局 ⇒ `draw` |

限时 **180 秒**（`config.json` 的 `limitSec`）：到点还没出现 `RESULT=…` ⇒ 尺子判 `timeout`（= 没通关）。
另外：整局没打印过任何事件 = `no_report`（本体没跑起来）；进程没了 = `program_gone` / `script_gone`。

⚠ 那些打印是**尺子的特权**：脚本**不许读**容器日志、不许读 `game/**` / `binary/**` 里本体的源码（读了就是开卷，
尺子也就成了空指标）。红线的完整版见最后一节。

## 脚本能用什么（就这些）

`__jz.computeruse.*`（类型声明在 `play/jzapi.d.ts`，语义与 C++ 一致）：

| 成员 | 说明 |
| --- | --- |
| `screenshot()` | 整块桌面的 PNG data URL ——"看"的唯一入口（想存帧就给 `fs` 写盘） |
| `click(x, y, btn)` | 归一化坐标（0~1，相对**整块桌面**）；**三个参数都不能省**（`btn` = `'left'/'right'/'middle'`） |
| `match(locatorJson)` | 像素兜底查找：`{"kind":"color","color":"#rrggbb"}` 或 `{"kind":"ocr","text":"…"}`；**可带 `region`**：`{"kind":"color","color":"#111111","region":{"x":…,"y":…,"w":…,"h":…}}` |
| `layoutTree(json)` | 目标窗口的可访问布局（`{"title":"五子棋"}`）；找不到窗口返回 null —— 可当"窗口在不在" |
| `ocrReady()` / `ocrTexts()` | 本地 OCR（本机没接后端，一律"未找到"——别指望它读状态栏） |
| `key(name)` / `type(text)` / `wait(ms)` | 键盘 / 打字 / 阻塞等（游戏在另一个线程照跑） |
| `waitStable(optsJson)` / `launch(optsJson)` | 等画面稳定 / 启动程序（虚拟桌面下 launch 不适用） |

另外：`fs` / `buffer`、`setTimeout/setInterval`、`console`。

**关于"怎么看见棋盘"**（这是本玩法的核心难点）：

1. `match({kind:'color', color:'#e3b572'})` → 最大的那块棋盘底色 = **棋盘画布矩形**（归一化），
   用它校准上面那两行公式（`box.x + 20/600*box.w + …`）——比硬编码像素抗偏移；
2. 想读**某个交点**有没有子：以该点为心、**躲开网格十字**（网格线是 `#3a2a12`）取一个 ~10x10px 的小
   `region`，然后 `match({kind:'color', color:'#111111', region:…})` 命中 ⇒ 那儿是黑子；
   换 `#f6f6f6` ⇒ 白子；两个都不中 ⇒ 空点。⚠ `match` 一次只回**一个**框（不是列表）⇒ 逐点问。
3. 所有坐标（`match` 的框 / `click` 的入参）基准都是**整块桌面**，可直接互用。

## 参数从哪来

运行根（= 你的 js 所在目录 `build/play/`）下的 `play.json`，由驱动/尺子写：

```json
{ "out": "<产物目录绝对路径>", "app": "gomoku", "play": "gomoku", "window": "五子棋",
  "warmupSec": 1.5, "steps": 900, "stepMs": 1000, "maxSeconds": 180 }
```

按 `maxSeconds` 收工（0 = 不看）；`steps`/`stepMs` 只是给你参考的观察节奏。

## 要交什么

1. **`play/play.ts`**（唯一产出；编译由驱动做，你只管把 TS 写对 —— 编译不过驱动会告诉你）；
2. 收工时写 `<out>/session.json`（`reason` / `steps` / `moves` / `result` / `frames` / `errors`）—— 没写 = 脚本卡死；
3. 隔几手把 `cu.screenshot()` 存到 `<out>/frames/`（给人、给下一轮的你看画面）。

## 红线

- ✅ 只**看画面**（`screenshot` / `match`）、只**操作鼠标键盘**（`click` / `key` / `wait`）。
- ⛔ 不读游戏内部状态：容器日志、状态文件、控件树、本体源码（`binary/code_agent/runtime/gomoku/**`）、模拟器内存。
- ⛔ 不改 `game/**`、`verify.py`、本体的任何文件。
- ✅ 随便改 `auto_code/game_play/play/**`（你自己写的东西）。

## 证据在哪（下一轮开头会给你）

`runs/current/`：`play.log` + `frames/`（脚本自留）、`external/` + `metrics.jsonl`（驱动从**外面**抓的帧与帧间差异）、
`report.md` + `result.json`（尺子的一页结论）、`container_stdout.txt`（容器日志：本体打印的 `[gomoku]` 事件行也在里面）。
