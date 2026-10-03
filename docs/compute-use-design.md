# Compute Use 设计

> 2026-09-25 成文；2026-09-29 收敛为「**建模期用像素，运行期用布局**」，并按用户要求**精简**：
> 运行时闸门 / 限流等策略**最后再做**（§7 第 8 条）。
> 参照文档：`widget-binding-design.md`（类在 JS、C++ 只出函数的同款决定）、`ai-native-design.md`。

## 0. 一句话

把"人操作界面"固化成**可精确重复**的脚本，而不是每步重新截图重推理。

**两条铁律**：

1. **像素只在建模期出现**：建树时同时产出「**像素 ↔ 布局映射**」；运行期只按**布局路径**寻址。
   像素可以告诉你"**变了没 / 长什么样**"，不可以告诉你"**控件在哪**"。
2. **界面不匹配先本地分级**，稳定复现才升级大模型；模型只写 **override**，**模型文件运行期只读**。

**分层**：

| 层 | 位置 | 职责 |
| --- | --- | --- |
| JS | `js_wrapper/computeruse` | 控件树 + UI 操作 + **校验**。`app` = 引用本包的一组**脚本**（包内不做框架 / 单例） |
| C++ | `JZComputerUse` / `JZJSComputerUseModule` | 只出 OS 原语，不建树 |
| 附带 | 同一条树 | 网页元素当普通节点，不立 `Browser` / `WebElement` |

## 1. 用例：打开 / 关闭电源

目标程序是**外部电源软件**（第三方上位机，只有像素和它自己的窗口布局）。

### 1.1 建模期（一次，人在环中）—— **像素在这里**

```ts
const shot = await captureScreen();                        // ① 截一张
const per  = await perceive(shot, { vlm });                // ② VLM：屏幕上有什么
const per2 = await review(per);                            // ③ 人审：删错检 / 补漏检 / 改类型文字
const app  = App.build(per2, { layout: layoutTree() });    // ④ 把元素认领到布局节点 ⇒ 映射
await app.save('psu.json');                                // ⑤ 固化：树 + layout 路径 + 页面指纹
```

`layoutTree()` 对自家界面就是 `dumpSemanticTree`：每个节点自带 `viewId`（= `objectName()`），
所以 `layout` 路径 = 从窗口往下拼 `viewId`（如 `/main/btnOutput`）；同一棵树还给出屏幕坐标与
`text / checkable+checked / range{value,min,max}` —— 运行期的**寻址**与**校验**都用它。
（`perceive` / `build(per,{layout})` 已实现，见 §7；§1.1 的 `review` 钩子已留、UI 待补。）

### 1.2 运行期（无人）—— **不碰像素**

```ts
// psu.ts —— app = 引用 computeruse 的一组脚本
import { App, expect } from 'computeruse';

const app = App.load('psu.json');                    // 模型：树 + layout 路径 + 页面指纹
await app.launch({ exe: 'PSU.exe', title: /电源/ });  // ① 启动 / 激活目标窗口
await app.attach();                                  // ② 绑定布局（不涉像素）

// 打开电源
await app.at('/main/btnOutput').click();             // ③ 布局寻址 → 注入
await app.expect('/main/lblOutput', 'ON');           // ④ 校验：读布局文本，不 OCR

// 关闭电源
await app.at('/main/btnOutput').click();
await app.expect('/main/lblOutput', 'OFF');
```

业务动作（发 SCPI 命令、读数据、写日志）**不走界面**，直接走协议 / 命令行；
界面层只做"只有人才能做的那部分"。

### 1.3 界面不匹配：先本地判，再决定要不要叫模型

```ts
try {
  await app.at('/main/btnOutput').click();
  await app.expect('/main/lblOutput', 'ON');
} catch (e) {
  // 分级（不调模型）：等界面稳定 → 重试 → 关掉意外弹窗 → 再重试
  // 仍稳定复现 ⇒ 升级模型：证据包 = 期望(expect + 页面指纹) + 实际布局树 + 历史(重试次数/上次结论)
  //    模型答：定性（偶发 / 真错）+ 修法（重试 / 补前置 / 换路径 / 更新映射）
  // 模型结论只落 override（本次生效）；要改模型文件必须显式合并（人工确认 / 再建模）
}
```

| 现象 | 性质 | 处置 |
| --- | --- | --- |
| A 暂时找不到（稳定 + 轮询 N 次内出现） | 偶发（时序） | 等待 + 重试 |
| B 找到但 `enabled` / `checked` / `range` 不满足前置 | 流程前置缺失 | 补前置步骤 |
| C 多出弹窗 / 对话框 | 偶发可自动处理 | 本地关掉后重试 |
| D 稳定复现缺失 + **页面指纹变了** | 真错：走错流程 / 换了界面 | ⭐ 升级模型 |
| E 同页但**节点路径变了** | 真错：版本漂移 | ⭐ 升级模型（要新路径 ⇒ 更新映射） |
| F 窗口 / 进程消失 | 真错：程序异常 | 重启或中止 |

⭐ **偶发 vs 真错 = 重试后是否稳定复现**，不靠模型拍脑袋；模型只在"稳定复现、但不知道该怎么修"时被叫来。

⚠ 上位机会驱动**真实硬件**（电源 / 示波器 / MES）：把 error 误判成偶发而继续点 ≈ 事故，
反之只是白停一次 ⇒ 默认**保守：拿不准就停、留证、回到可恢复点**，绝不"猜着往下点"。

⇒ 因此模型里必须记：每个**页面**一个**指纹**（窗口标识 + 关键节点路径集合），每个**步骤**一组**后置条件**（`expect`）。

## 2. 两阶段

| 阶段 | 输入 | 寻址依据 | 碰像素 |
| --- | --- | --- | --- |
| 建模期 | 截图 + 感知 + 人审 | 建立**像素 ↔ 布局映射** | ✅ 就是干这个的 |
| 运行期 | 固化模型 | **布局路径**（控件当前几何） | ❌ 不用于寻址（同步 / 证据 / 兜底除外） |

前提：**正常 app 都有布局** —— 自家有语义树；外部软件只要有 Win32 子窗口 / UIA / MSAA 就有。
"控件在哪"是**能问出来的事实**，用像素猜是多余的 ⇒ 窗口挪位 / 缩放 / 换主题 / 换 DPI 都不必重标定。

三条推论：

1. **映射是建模产物的一部分**：每个节点记 `layout` 路径，像素 rect 只作证据（人标注时看得见）。
   人审 / 标注的产物就是这个映射 —— 把"感知框"**认领**到布局节点上。
2. **退化必须显式**：没有可访问布局（自绘 / Canvas / 游戏 / 远桌面）才允许用像素族 locator，并在模型里标出。
3. **与评测不冲突**：评"感知能力（像素→树）"时禁用 `get_ui_state` / `dumpSemanticTree`（那是答案本身）；
   运行期控制用布局恰好是设计目标 —— 两条线评的不是同一件事。

## 3. Locator

```ts
export type Locator =
  | { kind: 'layout'; path: string }                             // ⭐ 运行期正路（待落 types.ts）
  | { kind: 'ocr'  ; text: string;  region?: Rect }              // 像素族：建模期辅助 + 无布局时兜底
  | { kind: 'image'; image: string; region?: Rect }
  | { kind: 'color'; color: string; region?: Rect; tol?: number }
  | { kind: 'yolo' ; model: string; label?: string; region?: Rect };
```

像素族的取舍（仍然成立，但已降级）：有文字 → `ocr`；图标 → `image`（单尺度，不做多尺度）；
色块 → `color`；复杂图标 → `yolo`。`region` 是**物理像素**小搜索区（与截图/click 同一口径），不传则搜整屏。

`click()` 契约不变：**先 `locate()` 判存在**（`index.ts:94-99`）**→ 找不到抛异常 → 找到才注入**
（`index.ts:102-107`）。

## 4. 校验（在 computeruse 包内）

- 文本 / 勾选 / 范围值**先问布局**（`aiText` / `aiChecked` / `aiRangeValue`，`JZWidget.h:364-373`）；
- 布局问不到才退像素（OCR = 本地 PP-OCR / 颜色 / 模板）；**OCR 只识别文字，状态判定仍优先问布局**。

⇒ 布局通道是"读字符串比较"，像素通道是"截图 + OCR"，成本差一个量级。

## 5. 分层边界

- **JS 侧**：`types.ts` 只有类型；`index.ts` 只有类与操作，所有副作用经 `backend`（= `__jz.computeruse`）；
  `setBackend` 是注入点，后端可整体替换（换平台 / 换测试桩）。
- **C++ 侧**：`JZComputerUse` 出 `switchToDesktop / click / drag / key / type / screenshot / layoutTree /
  matchImage / matchColor / matchText / ocrTexts / waitStable / launch`；`JZJSComputerUseModule` 建
  `__jz.computeruse`（`match` 收 `JSON.stringify(Locator)` 并按 kind 分派：`layout` **不在这里** ——
  路径规则只在 JS 侧；`image` / `color` / `ocr` 已接，`yolo` 未接）。
  `saveModel` / `loadModel` **不在 C++**：JS 侧序列化后经 `__jz.core` 读写文件。
  ✅ 两个类都已进 CMake（编在 `binary/code_agent/runtime`）。`ocr` 走 **JZCv（本地 PP-OCR, ONNX Runtime）**：
  用前 `__jz.computeruse.initOcr({modelRoot})` 或设 `JZ_CU_OCR_MODEL`；虚拟桌面下 OCR **直接读桌面合成帧**（不抓屏）。
- 为什么这样切：C++ 不碰控件树（那是业务模型），JS 不碰系统 API（副作用全走 backend）。

## 6. API 速查

**既有**：后端 `match / click / drag / key / type / wait / screenshot`；
`App.explore / fromPerception / load / save`；`key / type / wait` 作为 primitive 直接导出（`index.ts:251-253`）。
控件词汇（对齐 JZWidget）：`app / tab / menu / toolbar / group / window / button / link /
edit / combobox / checkbox / slider / label / canvas`。

**待补（名字待定；一律布局优先）**：

| 方法 | 语义 |
| --- | --- |
| `layoutTree()`（后端） | 导出可访问布局树（自家 = `dumpSemanticTree`；外部 = **UI Automation 优先**，退路 Win32 子窗口枚举） |
| `App.attach()` | 绑定窗口 / 布局（`img` 只用于定位窗口与退化兜底） |
| `App.at(path)` / `find(type,label)` | 取节点（按 layout 路径寻址） |
| `read(node)` | 读状态：文本 / 勾选 / 范围值 |
| `expect(node, matcher)` | 断言当前状态，不满足即抛（脚本级自检，对齐 `verify.py` 的硬门思路） |
| `exists(node)` | 存在性（= `locate() !== null`） |
| `sync({settle})` | 等界面稳定（像素在这里只当**信号**） |
| `launch({exe,title})` | 启动 / 激活目标窗口 |

## 7. 现状与待补

**已完成**：

- 包内文件：`types.ts` / `index.ts` / `perceive.ts` / **`layout.ts`**（路径 / 拍平 / 指纹 / 认领）/
  **`diagnose.ts`**（本地分级）；`explore = screenshot → perceive → 人审 → 认领布局 → 树`；
  VLM 地址由调用方给、不内置；包已是引擎内置名（`import { App } from 'computeruse'`）。
- ⭐ **§4.1 的布局通道、运行期寻址、校验、§1.3 的本地分级** 已在 `computeruse` 包内落地：
  `Locator.kind='layout'`、`backend.layoutTree()`、`App.attach / at / read / expect / sync / launch / has / diagnose`、
  `App.build(per,{layout})` 的**认领**、序列化带上 `layout / degraded / fingerprint`。
  **离线就能验证**（不需要模型、不需要真桌面、不需要 C++ 侧）：
  `auto_code/computer_use/checks/layout_smoke.ts` 用"电源开/关"当用例，14 项硬门全过 ——
  其中 G4 专门断言"整个流程里 `match` 只收到 `kind='layout'`"（铁律①的硬门）。
- 脚手架 `auto_code/computer_use/`（出题 → 截图 → 探针 → 判分，五项硬门全过）。

**待补（按依赖顺序）**：

1. **C++ 侧布局通道**：`JZComputerUse::layoutTree()` + `waitStable` + `launch`。
   - 自家窗口 = `dumpSemanticTree`（`source='semantic'`）；
   - **外部软件 = UI Automation**（`source='uia'`，2026-10-02 接上）：应用自己暴露的名字 / 角色 /
     值 / 可操作模式（节点里给出 `actions: invoke|toggle|select|setValue|expand`）—— 浏览器 /
     Electron / WPF 这类自绘界面只有它说得清；拿不到（COM 失败 / 目标没实现 UIA / 只有空壳）**才**
     退回递归枚举 Win32 子窗口（`source='win32'`）。两条都给不出东西 ⇒ 返回 null，JS 侧标
     `degraded` 退到几何 / 像素兜底。
   - ⚠ 虚拟桌面下 UIA 够不到（虚拟窗口没有原生 HWND）⇒ 那条腿明确返回空，不假装成功。
   - 仍未接：MSAA（只在"老应用只实现 IAccessible"时才有增量）；"按无障碍名字直接 Invoke"
     （现在靠 `rect` + 现有 `click` 已经够用，要更稳的动作语义再接）。
2. ✅ `JZComputerUse` / `JZJSComputerUseModule` 已进 CMake；像素族后端：`ocr` 已接（JZCv / PP-OCR，
   ONNX Runtime），`image` / `color` 需 OpenCV（`cmake/opencv.cmake`），`yolo` 待接。
3. 建模期的人审/标注 **UI**：`explore` 的 `review` 钩子已留（默认不传 = 行为不变），缺全屏标注窗。
4. 接大模型：`formatEvidence()` 已能产出证据包，缺"发给模型 → 解析修法 → 写 **override** 层"。
5. 像素族后端：✅ `ocr` 已接（本地 PP-OCR，`matchText`/`ocrTexts`）；`yolo`（小检测模型）仍未接。
6. ⏳ **闸门（预算 / 去重 / 影响面 / 无人值守）—— 最后再做。**

## 8. 参考

主流 CU 是"每步重截图重推理"（Claude Computer Use / Operator）；我们是
"**建模期一次生成像素 ↔ 布局映射 → 运行期按布局回放**"。相关范式：OmniParser（截图→元素）、
AppAgent（探索 App → 固化知识树）、SikuliX（模板匹配回放，对应我们的退化兜底）。
