# computer_use 的自动化修改流程（设计）

> 一句话：把"改 `js_wrapper/computeruse` 的实现"变成**无人值守的一轮轮实验** ——
> 一轮 = 一次可复现的实验（出题 → 截图 → 探索 → 判分），失败输出当下一轮的输入，
> 数字进 `baseline.json`。本文只写**流程与边界**（怎么接、谁负责什么、什么算通过）；
> 各脚本的细节看它们自己的头注释。

## 0. 与现状的关系

**已有、不动**：`run.py`（一条命令跑完六步）、`verify.py`（五项硬门 + 逐节点 diff，通过打
`[loop-stop]`）、`scenes/gen.ts`（出题 + 同源真值）、`probe/explore.ts`（固定探针入口）、
`agent_loop.py`（无人值守驱动：逐轮 POST /task + 收工复验 + 崩了重拉）。

**新增**：`binary/virtual_device` 的 HTTP 接口（`JZVirtualDeviceHttp.h`）—— 一台**无头容器**：
加载程序 / 截图 / 收鼠标键盘。它把"屏幕"从"真窗口 + 抓屏"换成"合成帧 + 容器坐标"。

为什么值得换（现状的三个约束 → 容器的四个确定）：

| 现状（`mcp.py` + `ui_screenshot`） | 换成容器之后 |
| --- | --- |
| 要有真桌面（`code_runtime --debug` 起真窗口），CI/服务器上跑不了 | `--headless`：不出界面，进程起来就能跑 |
| 截图是客户区但**2 倍缩放**，驱动得自己算 scale；受遮挡/前台/DPI 影响 | 无头下 scale 恒 1 ⇒ **PNG 尺寸 == 客户区逻辑尺寸**，与真值 rect 同一坐标系，不用换算 |
| 只给"语义调用"（`ui_invoke`），测不出"看不懂屏幕就点不中" | 像素是像素、输入是坐标（`/api/screen` + `/api/input/*`）⇒ 才是 CU 真正要考的能力 |
| 要 hold 住进程 + `kill_tree` 收尾 | 一个容器能同时跑多个程序（每个程序一个线程 + 一份 JS 运行时），删程序即收尾 |

## 1. 三个角色（别混）

| 角色 | 是什么 | 允许知道什么 |
| --- | --- | --- |
| 被测程序（场景 App） | 容器里的一个程序（`POST /api/programs`），JS 里 `new JZWindow(...)` | 什么都不知道；它只是"要被人看懂的界面" |
| 观察者（探针） | `probe/explore.ts` → `App.explore()` | **只有像素 + 窗口几何**（标题/外框/客户区）；⛔ 不许读控件树/内部状态 |
| 尺子 | `verify.py` + `scenes/truth.json` | 真值 + 五项硬门 |
| 实验台 | `run.py` | 把上面三者串成一轮，产物归档 |
| 驱动 | `agent_loop.py` + `agent_cli` | 轮次/复跑/收工；**不**理解指标，只看退出码与 `[loop-stop]` |

```
   ┌─────────── 容器 virtual_device --headless --http-port N ───────────┐
   │  程序: scene (JS, 一个线程 + 一份 JS 运行时)                        │
   │    ← POST /api/input/*   要"真动作"时才用                          │
   │    → 合成帧（客户区 PNG / 整屏 PNG）                                │
   └───────────────┬─────────────────────────────┬─────────────────────┘
     GET /api/screen?window=…            GET /api/windows
                   ↓                             ↓
        runs/current/shot.png ──────→ probe/explore.ts ─→ tree.json ─→ verify.py
        （客户区, 逻辑像素,            （离线后端: 只换设备原语）      （五项硬门 + diff）
          scale=1）                                                        │
                                            agent_loop.py ←── PASS? / diff ─┘
```

## 2. 容器：怎么起、给什么

```powershell
build\binary\bin\Debug\virtual_device.exe --headless --http-port 8720 --desktop 800x600
```

- `--headless`：不显示宿主界面、桌面干净、**没有窗口了也不退出**（容器的生命周期由驱动决定）。
- 接口（`JZVirtualDeviceHttp.h` 是唯一权威列表，这里只列与本流程相关的）：
  - `POST /api/programs {name, file|script, workDir}` 加载被测程序 —— `file` = 一个 **JS 程序**
    （按 ESM 模块加载：`import` 可用，运行根 = 它所在目录），`script` = 一段代码；回包里直接带上
    它建出来的窗口（命令行同款：`--program app.js`，见 `main.cpp --help`）；
  - `GET /api/windows[?process=&title=&visible=&at=x,y]` 窗口表：标题 + 外框/客户区几何
    （**逻辑像素**）+ 归属程序名 + **稳定句柄 `id`**（与 HWND 同角色；下标会随建窗/关窗变，
    长期引用请用句柄）。`at=x,y` 等价 WindowFromPoint，顺带给出该点在客户区里的坐标；
  - `GET /api/windows/<句柄|下标|标题>` / `POST /api/windows/<w>/state`
    （`activate|minimize|maximize|restore|close` + 移动/改客户区尺寸/改标题）、`DELETE /api/windows/<w>`；
  - `GET /api/screen?window=<句柄|下标|标题>` 客户区 PNG（不给 `window` 就是整屏）；
  - 输入有**两条**路（与真系统同一套对照，见 `JZVirtualDeviceHttp.h`）：
    `POST /api/input/mouse|key` 走桌面（**桌面**坐标 → 命中 + 激活 + 按区域分发，等价人拿鼠标点）；
    `POST /api/windows/<w>/input` 直接给那个窗口（默认**客户区**坐标、**不激活它**，等价 SendMessage）；
  - `GET /api/health`、`POST /api/desktop {scale}`、`DELETE /api/programs/<name>`。
    ⚠ 探针只许用 `screen` / `windows`（像素 + 窗口几何 + 句柄）；`state` 是**驱动侧**的工具
    （改窗口状态），不许进探针的输入。
- 两条纪律（选它就是为了这两条）：
  1. 截图给客户区 ⇒ **与真值 rect、命中测试同一个坐标系**，判分口径一个字都不用改；
  2. **容器没有"控件树"接口** ⇒ 想开卷也没有开卷的口子（`get_ui_state` 那种就是答案本身）。
    ⚠ 同理不留"在跑的程序里塞一段代码"的口子（曾有 `POST /api/programs/<name>/eval`，已删）——
     容器能读到的只有像素 + 窗口几何，读内部状态的路一律不加，这条不留给任何"自检"借口。

## 3. 一轮的八步（命令级）

```powershell
# 0) 改了 TS 才需要
<tsc> -p auto_code\computer_use\tsconfig.json

# 1) 起容器（常驻；端口由驱动挑空闲的）
virtual_device.exe --headless --http-port 8720 --desktop 800x600

# 2) 出题：场景 = 容器里的一个程序（gen.ts 同源写出 scenes/truth.json）
POST /api/programs   {"name":"scene","file":"build/scenes/gen.js","workDir":"build/scenes"}

# 3) 定窗口：真值与截图都以"客户区"为准
GET  /api/windows                     → 客户区 [x,y,w,h]（逻辑像素）

# 4) 取屏幕 → 探针的输入
GET  /api/screen?window=<场景窗口标题>  → runs/current/shot.png（尺寸 == 客户区逻辑尺寸）

# 5) 探索（探针照旧前台跑一次；它只读文件，不必进容器）
<runtime> --entry build/probe/explore.js         # 读 probe.json: screenshot/window/shot/out/vlm

# 6) 判分 + 归档
python verify.py        # 五项硬门 + 逐节点 diff；PASS 打 [loop-stop]
python run.py           # 上面六步串起来，并把产物归到 runs/round_NNN + scores.jsonl

# 7) 改代码：agent 按 diff 改**一处**（红线见 §4）

# 8) 回归：同批种子一起跑（baseline.json 的 regression 列表），全过才算进步
```

## 4. 谁改哪里（边界）

- ✅ 主战场 `JZJSEngine/js_wrapper/computeruse/**`；确实缺底层能力时 `JZWidget/`、`JZComputerUse/`
  （它们在模块里，**改完必须重编**）。
- ⛔ 题目与尺子：`auto_code/computer_use/scenes/**`、`verify.py`、`baseline.json`；
  ⛔ 固定入口：`probe/explore.ts`（改实现，不要改探针来"绕过"）。
- ⛔ 感知通路只许用**像素 + 窗口几何**。任何"控件树"/读程序内部状态的接口一律不许出现在探针的
  输入里 —— 这不是"风格"问题：一旦读了内部状态，五项硬门就全成了空指标。
- 一轮只改一处：diff 才能说清是哪一处起作用；一处改动配一次完整复跑。

## 5. 无人值守怎么接（`agent_loop.py`）

```bash
python3 agent_loop.py \
  --agent-cmd  "build/binary/bin/Release/agent_cli.exe -c <config.json>" \
  --prompt-file auto_code/computer_use/prompt.md \
  --verify-cmd  "python run.py" \                # 每轮：编译→出题→截图→探索→判分→归档
  --stop-tag    "[loop-stop]" \                  # verify.py 五项硬门齐过时才打
  --recheck-cmd "python run.py --no-archive"     # 收工前外部复验（只判定不落盘）
```

- **一轮** = 一次 POST /task（prompt + verify_cmd）：agent 干活 → 跑验证 → 退出码≠0 就把输出
  **原样喂回**续跑；退出码=0 则回到驱动，驱动看 `[loop-stop]` 决定收不收工。
- **失败信息就是下一轮的输入**：`verify.py` 打的是逐节点 diff（漏检/虚警/文本不符/类型不符/
  父子不符），驱动不看指标，模型按它改。
- **崩了重拉**：`agent_cli` 被杀/崩/端口没起来 → 等 `retry-sec` 重拉接着提交，工作区不丢。
- 轮次与重试上限在 `task.json`（`rounds.max=20`、`retrySec=5`）。

## 6. 抗过拟合与回归

- 每轮换种子（`run.py --seed N --level L`）；每 N 轮把 `baseline.json` 里那批种子**整批**跑一遍。
- 只有"新种子 + 回归批"都过才算进步；单题通过可能只是把题目背下来了。
- 门槛（`gates`）与数字只写在 `baseline.json`（进步的唯一凭据），产物 `runs/` 不入库；
  改口径要 `verify.py` 与 `baseline.json` 两边一起改。
- 题面口径：只收**有可见边框**的控件（`PushButton`/`LineEdit`）；无边框控件（`Label`/`CheckBox`/
  `RadioButton`）要先定"可见框"口径才收。

## 7. 接入时要动的地方（清单）—— 已全部落地

1. ~~**脚本的"运行根"**~~ ✅ **已解决**：容器现在按 **ESM 模块**加载程序
   （`POST /api/programs {file}` / `--program`）—— 走引擎的一站式 `init(运行根, 入口)`，
   于是 `import` 能用，且 `jzApp.applicationDirPath()` = **程序所在目录**、`applicationFilePath()`
   就是那份 js（脚本按入口目录找随它走的资源，这条约定成立）。用 `{script}` 才是 eval 一段代码
   （片段用，没有入口目录）。运行期包（fs/path/http…）按 `js_wrapper/<包>/dist` 部署在 exe 边上。
2. ~~`run.py` 加 `--container` 模式~~ ✅ **已做**：起容器 → `POST /api/programs`（场景按 JS 程序加载）
   → `GET /api/windows?process=scene`（等窗口出现, 取 `client` 客户区几何）→
   `GET /api/screen?window=<句柄>` 存 `runs/current/shot.png`；探针/判分/归档那几步一行没改。
   `mcp.py` 那条真窗口路 **保留**（要在真桌面上对照验证时还得用它）。
3. ~~容器收尾~~ ✅ **已做**：`finally` 里先 `DELETE /api/programs/<name>`（连带它的窗口），
   再 `kill_tree` 收容器进程（无头下它不会自己退 —— 见 `main.cpp` 的 `setQuitOnLastWindowClosed(false)`）。
4. ~~`task.json` 补几个字段~~ ✅ **已做**：新增 `container { desktop, program }`，`run.py` 从它读
   （缺省 `1024x768` / `scene`）；容器 exe 按 `--cfg` 推导为
   `build/binary/bin/<cfg>/virtual_device.exe`（`--container-exe` 可覆盖）。
5. ~~判分口径**不用改**~~ ✅ **已确认**（真值与预测都是逻辑像素）—— 这就是选容器的主要理由。

### 实测（Debug, seed=0 level=0, verify 用 --no-vlm ⇒ 预期红灯）

| | 真窗口路（默认） | 容器路（`--container`） |
| --- | --- | --- |
| 屏幕从哪来 | `code_runtime --debug --ui-mcp-port N` + `ui_screenshot` | `virtual_device.exe --headless --http-port N --desktop 1024x768` |
| 窗口客户区 | 420x320 | 420x320（`client=[302,220,420,320]`） |
| 截图 PNG | 525x400（scale 随宿主 DPI, 本机 1.25；不是固定倍数） | **420x320（无头下 scale 恒 1）** |
| 需要真桌面 | 是 | 否 |

容器截图另做过一次**像素级抽查**：非背景像素包围盒 `[x=12 y=40 w=398 h=104]` 与真值可见框
并集（edit `12,40,396,40` + 一行 button 到 `y=144`）**一像素不差** ⇒ 客户区截图与
`truth.json` 的 rect 确实是同一坐标系（§2 的两条纪律成立），判分口径不用动。

## 8. 还没做的 / 风险

- 容器里的"真动作"（`POST /api/input/*`）目前只用于自检（见 `binary/virtual_device/api_test.py`）；
  题目上还没有"需要动手才能看清"的用例（L3 隐藏结构）—— 要做时把探针的离线后端换成"打到容器"，
  那时 `explore()` 的点击/输入才真落在界面上。
- 多窗口/多程序用例、L2 主题/DPI（`POST /api/desktop {scale}` 已能造 DPI 场景）都还没有判定口径。
- 容器刻意不给控件树：将来若要做"布局树 vs 像素"的对照实验，得另开一个**只给驱动**的口子，
  并保证它不进探针输入。
