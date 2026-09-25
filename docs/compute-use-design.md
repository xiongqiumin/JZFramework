# Compute Use 设计目标

> 2026-09-25 首次成文。把"截图生成按键脚本、脚本执行重复操作"这一能力的设计目标、
> 分层边界、定位策略与未来形态写成判据，并对照**当前代码**给出实现现状。
> 参照文档：`widget-binding-design.md`（控件族的类定义在 JS 侧、C++ 只出函数的同款决定）、
> `js-engine-wrapper-design.md`（模块/包/名字的总原则）、`ai-native-design.md`。
>
> **结论先行**：Compute Use 只解决一件事 ——
> **把"人操作桌面/应用"固化成可重复执行的脚本**，回放以**确定性锚点**为主、以 LLM 感知为辅。
> 分层上严格对齐控件族已定的原则：
> ① 控件树与 UI 操作全在 **JS** 侧（`js_wrapper/computeruse`）；
> ② C++ 侧（`JZComputerUse` + `JZJSComputerUseModule`）**只提供底层 OS 语义**，不构建控件树；
> ③ 浏览器/网页只是**附带能力**，不单独建模成 `Browser`/`WebElement` 类。

---

## 1. 要解决什么问题

传统 Computer Use（Claude Computer Use / Operator / OmniParser 系）是
**每一步都重新截图、重新推理坐标**。优点是通用，缺点是：

- **脆**：窗口挪位、分辨率变化、主题色变，坐标全错；
- **不可复现**：同一任务跑两次结果可能不同，无法"精确重复 N 次"；
- **贵**：每步都调 VLM，延迟与成本都高。

本框架的目标场景（用户原话："截图生成按键脚本，脚本执行重复操作"）恰好是
**可重复、可复现的批处理**——例如"在画图软件里画 100 个红圆""每天把同一份报表导出发邮件"。
这类任务不需要每步重推理，只需要**一次标定、多次确定性回放**。

§9 给出对应开源范式，本设计是其中两类思路的融合。

---

## 2. 核心思想：持久锚点 + 确定性回放

把"控件在哪、长什么样"在**录制/探索期一次性**确定下来，固化为一棵**语义控件树**
（`App / Tab / Button / Canvas ...`，用 TS 类描述，见 `js_wrapper/computeruse/index.ts`）。
之后回放时：

1. 每个控件带一个 **Locator**（见 §4），声明"我靠什么被找到"；
2. 回放时先在**小区域**内按 Locator 定位（模板匹配 / OCR / 颜色 / 检测）；
3. 找到了 → 注入真实 OS 动作（点击/拖拽/按键）；找不到 → 抛"控件不存在"异常。

窗口挪位也不怕：Locator 在小区域内重定位，坐标自动跟随。**这正是 `Button.click()` 的契约**
（`index.ts:117-120`、`index.ts:93-98`）——先 `locate()` 判存在，找不到抛异常，找到才 `backend.click()`。

这与"每步重推理"形成根本区别：我们**不每步截图重猜**，只在锚点失效的异常分支才可能需要 LLM 介入
（未来 `App.explore` / `fromPerception` 的扩展点，现状未实现，见 `types.ts:72-73`）。

---

## 3. 分层边界（关键约束）

直接复用控件族在 `widget-binding-design.md` 里定下的原则：**类定义在 JS、C++ 只出函数**。
Compute Use 的分层如下：

### 3.1 JS 侧：控件树 + UI 操作（`js_wrapper/computeruse`）

- **`types.ts`** —— 纯接口契约（与实现解耦）：几何 `Rect/Point`、定位策略 `Locator`、
  底层后端接口 `ComputerUseBackend`、语义节点接口 `ICuNode/IButton/...`。
  这里**只有类型，没有逻辑**。
- **`index.ts`** —— 具体类 `implements` 接口，所有 OS 动作**100% 通过 `backend`（即 `__jz.computeruse`）**，
  自己不碰任何系统 API，也不在 JS 侧匹配图片/发键鼠。
- 控件词汇参考 `JZWidget`：`App / Tab / Menu / Toolbar / Group / Window / Button / Edit /
  ComboBox / CheckBox / Slider / Label / Link / Canvas`（见 §6 控件表）。
- 模型固化在 JS 侧：`serialize`/`deserialize`（`index.ts:249-290`）把控件树序列化成 JSON，
  经 `__jz.core` 落盘；`App.save` / `App.load` 复用之。

### 3.2 C++ 侧：只提供底层 OS 语义（`JZComputerUse`）

- **`JZComputerUse`** —— 基本 OS 语义的**纯 C++ 类**，不依赖引擎、`JZObject` 派生即可：
  `switchToDesktop`（Win+Tab / Win+D 切虚拟桌面/显示桌面）、`click`（归一化坐标 → 屏幕像素 → `SendInput`）、
  `drag`、`key`（名称 → `inputECoreToVK` 映射 → `SendInput`）、`type`、
  `screenshot`（复用 `JZScreenShot::captureVirtualDesktop` → dataURL）、
  `findImage`（OpenCV `matchTemplate` 单尺度）、`findColor`（`JZColorUtil`）。
  **不在 C++ 侧构建任何控件树**——树在 JS 侧。
- **`JZJSComputerUseModule`** —— 把 `JZComputerUse` 暴露给 JS，建 `__jz.computeruse`，
  提供 `switchToDesktop / click / drag / key / type / wait / screenshot / match / saveModel / loadModel`。
  `match` 统一收 `JSON.stringify(Locator)`，按 `kind` 分派到 `findImage`/`findColor`；
  其中 `ocr`/`yolo` 两种策略**当前未接入**（匹配失败返回未找到，待后续接 OCR/YOLO 后端）。
- **不进构建**：`JZComputerUse` / `JZJSComputerUseModule` 尚未加入任何 CMake，且不挂 `JZObject` 派生树
  （`JZComputerUse` 是独立 OS 工具类）。接入时再在 `binary/code_agent/CMakeLists.txt` 注册。

### 3.3 为什么这样切

- **C++ 不碰控件树**：控件树是"业务模型"（哪个按钮属于哪个选项卡），属于 JS 表达层；
  底层"点击某个像素""在屏上找张图"才是 OS 原语，归 C++。两侧职责清晰、互不阻塞。
- **JS 不碰系统 API**：所有副作用经由 `backend` 接口（`types.ts:53-74`），后端可整体替换
  （换平台、换注入实现、换测试桩），上层语义树一行不用改——`setBackend`（`index.ts:50`）即注入点。
- 这正好与控件族"JS 类层 + C++ 函数"的同款决定一致，不另立一套范式。

---

## 4. 定位策略 `Locator`（录制期决定"控件靠什么被找到"）

`types.ts:31-35` 把"怎么找控件"做成多态，而不是单一图片模板——对应三类控件的现实取舍：

```ts
export type Locator =
  | { kind: 'ocr';   text: string; region?: Rect }        // 基本文字按钮（OCR 找文字，最省事）
  | { kind: 'image'; image: string; region?: Rect }       // 图标按钮（模板匹配，单尺度）
  | { kind: 'color'; color: string; region?: Rect; tol?: number } // 颜色块（colorFind）
  | { kind: 'yolo';  model: string; label?: string; region?: Rect }; // 复杂图标（小检测模型）
```

设计取舍（用户原话）：

- **基本文字按钮 → `ocr`**：最省事，不需要准备任何素材；
- **图标按钮 → `image`**：单尺度模板匹配即可，**当前不做多尺度**（多尺度不在范围内，留待后端，不改 `Locator` 形状）；
- **颜色块 → `color`**：`colorFind` 按颜色，比模板更稳；
- **复杂图标 → `yolo`**：几个样本即可训小检测模型，"花不了多久"。

`region` 是可选的小搜索区域（归一化 `[0,1]`），不传则搜整屏。小区域带来两个好处：
① 同名图标不误命中；② 匹配更快。**"Button.clicked 要判断自己是否存在"即 `locate()` 在小区域内找**
（`index.ts:85-90`），找不到即视为"不存在"，抛异常而非静默继续。

加策略**不破坏现有节点模型**——只是 `Locator` 多一种 `kind`，控件树结构不变。

---

## 5. 确定性回放之外：录制/探索（未来形态，接口已留）

回放需要"树从哪来"。录制期职责（未来 `App.explore` / `fromPerception`，`types.ts:72-73`，现状未实现）：

1. 截图当前界面；
2. 用 VLM / 现有感知标出可交互锚点；
3. **为每个控件选定 `Locator` 的 `kind`**（启发式：有文字 → `ocr`；纯色区 → `color`；
   干净图标 → `image`；以上都不稳 → 收集样本训 `yolo`）；
4. 固化成 `App` 模型（即 `paintApp.ts` 那种 TS 类，或序列化后的 JSON），供回放复用。

这是一个自然的"录一次、跑百次"闭环，也是与 AppAgent（腾讯）同形的探索→固化→复用结构。

---

## 6. 控件词汇表（对齐 JZWidget）

| 控件 | type | 关键方法 | 定位/动作语义 |
| --- | --- | --- | --- |
| `App` | `app` | `find` / `button` / `save` / `load` / `explore` | 树根 |
| `Tab` / `Menu` | `tab` / `menu` | `activate` / `ensureActivated` | 激活型容器：先定位再点 |
| `Toolbar` / `Group` / `Window` / `Label` | 对应 | — | 纯容器/占位 |
| `Button` / `Link` | `button` / `link` | `click(btn?)` | 复用基类"先判存在再注入" |
| `Canvas` | `canvas` | `drag(from,to)` | 按坐标拖拽画形状 |
| `Edit` | `edit` | `setText` / `clear` | 点中 → 输入 / Ctrl+A+Delete |
| `ComboBox` | `combobox` | `select(label)` | 点开 → 输入 → 回车 |
| `CheckBox` | `checkbox` | `toggle/check/uncheck` | 当前默认点击切换（状态判定待 OCR/colorFind） |
| `Slider` | `slider` | `setFraction(f)` | 按水平比例拖拽（垂直待补） |

所有节点共用基类 `CuNode`（`index.ts:60-114`）的"定位→点击"语义；`Button` 严格按用户最初要求实现：
**构造传入 Locator（铅笔=图片/颜色/文字），clicked 时在小区域判存在，不存在即异常，存在才注入**。

---

## 7. 浏览器 + 桌面一体（附带能力）

用户明确：**不需要独立的 `Browser` / `WebElement` 类，这是附带功能**（`types.ts:149-150`）。

- 网页里的元素（链接、输入框、提交按钮）**直接当成普通节点**，用 `image/ocr/color/yolo` 定位即可，
  复用同一棵桌面控件树、同一套 `backend` 动作。
- 未来若需要 DOM 选择器，只需给 `Locator` 增一种 `kind: 'dom'`，**不引入新控件类**，
  保持"桌面与网页统一成同一棵树"的目标，但用最轻的方式落地。
- 因此 `ICuNode` 没有 `addBrowser` / `addWebElement`，实现里也没有 `Browser` / `Page` / `WebElement` 类。

---

## 8. 关键 API 契约（速查）

**后端接口**（`types.ts:53-74`，C++ `JZJSComputerUseModule` 必须实现）：

| 方法 | 语义 |
| --- | --- |
| `match(locator)` | 按定位策略在小区域查找，返回命中 `Rect` 或 `null`（单尺度） |
| `click(rect, btn?)` | 在 rect 中心注入鼠标点击 |
| `drag(from, to)` | 归一化坐标间拖拽 |
| `key(name)` | 按名字按键（`'Return'`/`'Ctrl+A'`/`'LWin'`…） |
| `type(text)` | 输入文本 |
| `wait(ms)` | 等待 |
| `screenshot()` | 虚拟桌面截图 → dataURL |
| `saveModel/loadModel/explore/fromPerception` | 模型固化/装配（JS 侧实现序列化） |

**底层 primitive**（`index.ts:242-244`）：`key` / `type` / `wait` 作为非树内通用动作直接导出。

---

## 9. 对应开源范式（设计不凭空造轮子）

- **回放半（图片锚点 + 小区域匹配）**：SikuliX（Java+OpenCV 模板匹配）、
  PyAutoGUI `locateOnScreen`、AutoHotkey `ImageSearch`、UI-Vision Kantu。
  我们的 `findImage` 单尺度匹配 + `region` 小区域，正是此范式的精简版。
- **录制半（VLM 生成锚点 / 语义树）**：OmniParser（微软，截图→可交互元素 bbox+标签）
  对应"录制期感知层"；AppAgent（腾讯，探索 App→固化可复用知识树）对应我们的 `paintApp` 用 TS 类描述 UI 树。
- **记录-回放 / RPA**：OpenAdapt（录屏+LLM 泛化重放）、Agent-S / Mobile-Agent（试错完成任务）。

**我们的独特点**：主流 Computer Use 是"每步重截图重推理"；我们是"**VLM 一次性生成持久锚点 → 之后确定性模板回放**"
——窗口挪位靠小区域重定位、任务可精确重复 N 次，契合"截图生成脚本、脚本重复操作"的目标。

---

## 10. 实现现状与待补（截至本文）

**已完成（TS 接口 + 实现 + C++ 桩，均未进构建）：**

- `js_wrapper/computeruse/types.ts`：完整接口契约（0 错误）。
- `js_wrapper/computeruse/index.ts`：控件树 + UI 操作 + `backend` 对接 `__jz.computeruse`（0 错误）。
- `JZComputerUse`（`JZCore/JZComputerUse.h/.cpp`）：`switchToDesktop/click/drag/key/type/screenshot/findImage/findColor`（未进构建）。
- `JZJSComputerUseModule`（`JZJSEngine/JZJSComputerUseModule.h/.cpp`）：暴露 `__jz.computeruse`（未进构建）。
- `test/compute_use/paintApp.ts`、`draw_red_circle.ts`：消费示例（独立 TS 环境解析不到包名，引擎运行时正常）。
- `docs/compute-use-design.md`：本设计文档。

**待补（按依赖顺序）：**

1. **接入构建**：把 `JZComputerUse` / `JZJSComputerUseModule` 加入 `binary/code_agent/CMakeLists.txt`，
   在引擎初始化时建 `__jz.computeruse`。
2. **补齐 Locator 后端**：`ocr`（接 OCR 引擎）、`yolo`（接检测模型）；当前 `match` 对这两种返回未找到。
3. **录制/探索**：实现 `App.explore` / `fromPerception`（VLM 标锚点 → 选 Locator kind → 固化模型）。
4. **控件状态判定**：`CheckBox` 的状态读取（OCR/colorFind）、`Slider` 垂直方向、`ComboBox` 的视觉展开校验。
5. **异常分支的 LLM 复核**（可选）：锚点失效时回退 VLM 判断"是否到了预期界面"，而非直接失败。
