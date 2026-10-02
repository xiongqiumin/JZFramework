# auto_code —— 自改进工作区（不参与构建）

> 本目录**不进 CMake、不含可编译代码、不参与构建**（与 `skill/`、`docs/` 同类）。
> 它是 agent 用来**生成/改进程序**的工作区：每个子目录 = 一个长期任务，
> 由 `binary/agent_cli`（+ `binary/agent_cli/tools/agent_loop.py`）驱动，人只审阅与决策。

## 一个子目录 = 一个任务的完整闭环

```
auto_code/<task>/
├─ task.json      # 任务定义：目标 / prompt / 断言命令 / 参数（给人或后续 run.py 读）
├─ prompt.md      # 给 agent 的任务说明 + 输出契约 + 允许改哪里
├─ verify.py      # 断言：打分 + 逐节点 diff + 达标打 [loop-stop]（--check-only 供外部复验）
├─ scenes/        # 场景（题目）：种子 → 场景 + 真值，**同源一份**
├─ probe/         # 探针：怎么调用被测功能（相对稳定）
├─ examples/      # 被测功能的消费示例（人看的最小例子）
├─ baseline.json  # 当前基线与硬门（数字要提交，这是"进步了多少"的唯一凭据）
└─ runs/          # ⚠ 产物，不入库（.gitignore 已排除）
```

## 三条铁律

1. **不参与构建**：这里出现 `CMakeLists.txt` / `.cpp` 就是放错了 —— 要编译的东西属于被它
   驱动的模块（`JZWidget/`、`JZJSEngine/`、`JZComputerUse/`）或 `test/`。
   这里只有"**程序 + 数据**"：TS 程序（`code_runtime` 跑）、Python 断言、JSON 任务定义、产物。
2. **直连源码路径，不拷副本**：命令一律指向本目录里的源码。⛔ 不要照
   `binary/code_agent/benchmark/data` 那样 POST_BUILD 拷到 `bin/` —— 那是给"题目分发"用的；
   工作区拷了副本就会出"改了源码忘了拷，测的是旧夹具"（产物过期=事故第一来源）。
3. **产物不入库**：`<task>/runs/` 已被 `.gitignore` 排除；只有 `baseline.json` 里的数字提交。

## 任务列表

| 任务 | 目标 | 状态 |
| --- | --- | --- |
| `computer_use/` | 让 `js_wrapper/computeruse` 的探索实现（`App.explore`）能从屏幕像素产出语义场景树 | ✅ 全链路绿灯（出题→截图→感知→装配→判分→归档）：8 个种子五项硬门全过，数字见 `baseline.json`；感知后端 = `perceive()` 打多模态接口（地址由驱动给）；⚠ 最短板 `iou75` 无余量。屏幕已有两条路：真窗口路（默认）/ **无头容器路**（`run.py --container`，虚拟桌面当容器，已接入并实测链路通） |
| `game_play/` | 交一个**一键通关脚本**：容器里**两个进程** —— 游戏（题目侧）+ `script`（脚本），脚本用 computer use 看画面/按键，把一个游戏**打通关**。**两种玩法**（`task.json` 的 `plays`）：`shooter`（默认，飞行射击 800x600）/ `gba`（模拟器 240x160，代码保留），`run.py --app` 切 | 🟡 链路全通、形态已定：**产出 = `play/play.ts`**（作为进程 `script` 跑在 virtual_device 里，用 `__jz.computeruse.*`：screenshot/click/key/wait），对面是进程 `gba`（题目侧 `game/game.ts`，不许改）；驱动只装这两个进程、**从外面抓帧当旁证**。实测 `python run.py` 一局全绿：脚本按 Start 把游戏从标题页推到主菜单（`round_011/external/e00013.png` 为证）。观测面 = 脚本的 `play.log`/`frames/` ＋ 驱动的 `external/`/`metrics.jsonl`/`report.md`。⚠ **不判"通关"**：`verify.py` 只判七道**链路/证据**硬门且**不打 `[loop-stop]`**（通关没有自动真值，收工由人看归档画面） |

## 怎么跑（以 computer_use 为例）

```powershell
cd auto_code\computer_use

python run.py                     # 编译 → 出题 → 截图 → 探索 → 判分 → 归档（一条命令）
python run.py --container         # 屏幕改用**无头虚拟桌面容器**（不需要真桌面, scale 恒 1）
python run.py --seed 7 --level 1  # 换题目（种子/难度）
python run.py --skip-build        # TS 没改，跳过 tsc
python run.py --no-shot           # 只出题/判分（不拉常驻假 App、不截图）
python run.py --no-vlm            # 不给感知后端（用来看"没后端要报错"这条契约）
python verify.py --self-test      # 只校尺子（真值当预测应满分）
```

链路里的三段（都在本目录，都不进构建）：
- 题目：`scenes/gen.ts`（一份声明 → `uiLoader` 真控件树 + 同源 `scenes/truth.json`）；
- 屏幕（两条路，用 `run.py --container` 切）：
  · **真窗口路**（默认）：场景进程**常驻**拉起（`code_runtime --debug --ui-mcp-port N`），
    `mcp.py` 走 `ui_screenshot` 取 PNG（尺寸随宿主 DPI，本机实测 525x400 = 1.25 倍，不是固定倍数；
    窗口逻辑尺寸从 `ui_windows` 的 bounds 取，真值用它同一坐标系）；
  · **容器路**（`--container`）：起 `virtual_device.exe --headless --http-port N --desktop WxH`，
    `POST /api/programs` 把场景当 **JS 程序**加载，`GET /api/screen?window=` 取客户区 PNG ——
    无头下 scale 恒 1 ⇒ **PNG 尺寸 == 客户区逻辑尺寸**，且不需要真桌面；
  ⚠ 两条路的截图都是**窗口客户区**（真窗口路里 `JZScreen::grabWindow` 会从整窗抓图里裁掉标题栏/边框）
  ——客户区坐标系才与控件 geometry / `hitTest` / 合成点击一致；
- 探针：`probe/explore.ts` 读 PNG + 窗口几何 + **驱动给的感知后端地址**（`--vlm-url`，默认本机
  llama.cpp 多模态端点；也可用环境变量 `JZ_VLM_URL`/`JZ_VLM_MODEL`）→ `setBackend()` 注入离线后端
  → `App.explore()` → 写 `runs/current/tree.json`。

题目口径（判分按 IoU 比框，所以是硬约束）：只用**有可见边框**的控件（`PushButton`/`LineEdit`）
⇒ 真值框 == 控件 geometry == 屏幕上看得见的框。`Label`/`CheckBox`/`RadioButton` 没有整体边框
（像素上只有"墨迹"，宽度/高度还随文字与行高变）⇒ 真值框无客观答案、属模糊判定，暂不进题目；
要收它们得先给可见边框（或定义"墨迹框"口径），那时优先补**带边框的容器**（GroupBox/TabWidget），
顺带把 `hierF1` 变成真指标。

产物全在 `runs/`：`current/`（本轮中间件）+ `round_NNN/`（每轮归档）+ `scores.jsonl`（趋势，一行一轮）。
`build/`（tsc 产物）与 `runs/` 都不入库。
