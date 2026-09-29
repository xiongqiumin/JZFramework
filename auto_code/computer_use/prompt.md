# 任务：让 computer-use 能"看懂"一个界面（产出语义场景树）

## 目标

改进 `JZJSEngine/js_wrapper/computeruse` 的探索能力，使 `App.explore()` 能从一个**只有像素**
的界面里产出语义场景树，并且与真值足够接近（判据见下）。

## 现状（动手前先看清楚）

- `explore()` 已经接通：**截图 → 感知 → 装配**三步（`js_wrapper/computeruse/index.ts` 的
  `backend.explore` + `buildTree`），感知层在 `js_wrapper/computeruse/perceive.ts`
  （打 OpenAI 兼容的**多模态**接口，地址由调用方的 `ExploreOptions.vlm` 给，本包不内置）。
  当前水平见 `baseline.json`（8 个种子全过；**最短板是 `iou75`，正好压在门槛上、没有余量**）。
- `__jz.computeruse` 只提供底层语义：`screenshot / match(image|color) / click / drag / key / type / wait`；
  `match` 的 `ocr` / `yolo` 两种 kind **未接入**（返回未找到）。离线探针里 `match` 是恒 null 的桩
  —— 感知不走它（走 `perceive()`）。
- 控件树模型（`App / Tab / Button / Edit / ...` + `serialize`）**已经实现**，可复用。
- 探针 `probe/explore.ts` 是固定入口：它调用你的实现，把结果写到
  `runs/current/tree.json`。**改实现，不要改探针来"绕过"**。
- ⚠ 两个坐标/口径前提（题目约定，别去"修"）：
  1. 截图是**窗口客户区**（不含标题栏/边框），坐标以它为准 —— 所以根窗口节点的文本是空串
     （标题在非客户区，像素里看不到）；
  2. 题目只用**有可见边框**的控件（`PushButton` / `LineEdit`）⇒ 真值框 == 控件 geometry ==
     屏幕上看得见的那个框。没有整体边框的控件（`Label` / `CheckBox` / `RadioButton`）真值框
     没有客观答案（像素上只有"墨迹"，宽度还随文字变），**属于模糊判定，不进题目**。

## 允许改哪里

- ✅ `JZJSEngine/js_wrapper/computeruse/**`（这是主战场）
- ✅ `JZJSEngine/js_wrapper/computeruse/types.ts` 的形状可以扩（加 `kind` 等），但别推翻已有契约
- ✅ `JZJSEngine/js_wrapper/<其它包>`、`JZWidget/`、`JZComputerUse/`（若确实需要补底层能力；
  注意这些在模块里，改完必须重编）

## 禁止改哪里

- ⛔ `auto_code/computer_use/scenes/**`（题目）
- ⛔ `auto_code/computer_use/verify.py`、`baseline.json`（尺子）
- ⛔ 不要为了通过而写死坐标/文本（场景由种子随机生成，真值现场算）

## 输出契约

`probe/explore.ts` 会把结果写成 JSON：

```json
{ "nodes": [
  { "id": "n1", "type": "button", "text": "7", "rect": [10, 60, 40, 30], "parent": "n0" }
] }
```

- `type` 用与 `computeruse` 包一致的词汇（`button` / `edit` / `checkbox` / `tab` / `canvas` …）
- `text` 是控件上的**可读文字**（看不到文字就留空串）
- `rect` = `[x, y, w, h]`，单位是与真值相同的逻辑像素
- `parent` = 父节点 `id`，根节点为 `null`

## 怎么跑

```powershell
cd auto_code\computer_use
python run.py                # 编译 → 出题(造场景+真值) → 探索(探针) → 判分 → 归档
python run.py --seed 7       # 换题目；同种子结果可复现
```

`run.py` 会调 `code_runtime` 跑 TS：`scenes/gen.ts` 造场景、`probe/explore.ts` 产出预测。
判分细节见下面一节；产物在 `runs/`。

## 判定（跑 `python verify.py`，或由 `run.py` 代跑）

先按矩形匹配节点（IoU ≥ 0.5），再看文本 / 类型 / 父子边；**五项硬门齐过**才算通过：

| 指标 | 含义 | 门槛 |
| --- | --- | --- |
| `detF1` | 节点发现 F1（漏检与虚警都要少） | ≥ 0.90 |
| `textAcc` | 匹配上的节点里文本正确的比例 | ≥ 0.95 |
| `typeAcc` | 匹配上的节点里类型（同族）正确的比例 | ≥ 0.90 |
| `hierF1` | 父子边集合的 F1 | ≥ 0.85 |
| `iou75` | 匹配上的节点里 IoU ≥ 0.75 的比例 | ≥ 0.60 |

## 失败信息怎么用

`verify.py` 失败时会打印逐节点 diff（漏检 / 虚警 / 文本不符 / 类型不符 / 父子不符），
**它就是你下一轮的输入** —— 按它指出的那一类问题去改实现，别同时改五处。

通过时脚本会打印 `[loop-stop]`，说明本轮可以收工。
