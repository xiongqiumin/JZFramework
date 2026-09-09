---
name: JZWidget 样式表使用指南 (stylesheet)
description: 讲解 JZWidget 的 CSS 子集样式表如何在 TS/JS 侧使用，以及与标准 CSS3 的差异。当用户要在 TS/JS 里给控件/全局写样式、做换肤主题、用 var() 令牌、确认某条 CSS 写法是否被支持、或排查"样式写了但界面没变化"时使用。
---

# JZWidget 样式表 (stylesheet) 使用指南

> 详细设计见 `docs/jzstylesheet-design.md` (v3.1)。
> 实现：解析 `JZWidget/JZStyleSheetParser.{h,cpp}`、值应用 `JZWidget/JZStyleSet.{h,cpp}`、
> 规则匹配 `JZWidget::resolvedStyle()`（`JZWidget.cpp`）。
> 本技能面向 TS/JS 使用方，不涉及 C++ 绑定细节。

## 一、核心结论（先读）

**不要把 `JZStyleSheetParser` 类暴露给 TS。** 它是纯解析器，职责只有 "CSS 文本 → 规则列表"；
规则匹配（resolve）由框架在每次绘制前自动完成并缓存，TS 侧不需要（也无法）直接驱动。

> **生效范围（先看这里）**：规则匹配与绘制已打通，但**只有已接入样式的控件类会响应 CSS** ——
> `JZWidget`（基类容器）/ `JZWindow` / `JZPushButton` / `JZLabel`。
> 其余控件（`JZTextEdit`、`JZListWidget`、`JZComboBox`…）外观仍是硬编码，写了样式也不变。详见 §七。

TS 侧真正能用的高层 API 是：

| 对象 | 方法 | 说明 |
|------|------|------|
| `JZApplication` | `setStyleSheet(css: string): boolean` | 全局 author 表，作用于所有控件 |
| `JZApplication` | `styleSheet(): string` | 原始 CSS 文本 |
| `JZApplication` | `styleSheetError(): string` | 上一次 `setStyleSheet` 失败原因 |
| `JZWidget` | `setStyleSheet(css: string): boolean` | 控件级表：作用于**本控件及其子孙**，优先级高于全局表 |
| `JZWidget` | `setStyleClass(cls: string): void` | 空格分隔的类名串，等价 HTML `class`（如 `"btn primary"`） |
| `JZWidget` | `styleClass(): string` | 当前类名串 |
| `JZWidget` | `styleSheet() / styleSheetError(): string` | 同上 |

`JZStyleSheetParser` 仅作为 `styleSheetParser()` 返回的内部只读解析结果，**不需要**在 TS 里直接 new 或调用。

## 二、概念模型（与 CSS 对齐的部分）

- **级联来源**：`UA 表`（引擎内置默认外观，当前为各控件的 C++ `uaStyle()`，尚未搬成内置 CSS 表）< `author 表`（`app.setStyleSheet`）<
  `inline`（`widget.setStyleSheet`）。`!important` 跨级覆盖。
- **特异性**：三元组 `(id, 类+伪类, 类型)`，按 `100 / 10 / 1` 折算；同特异性后者胜。
- **类型选择器 = 框架类名**：`JZPushButton` 在选择器里的角色等同 HTML 的 `<button>`。
- **`#id` = `objectName`**：控件无 DOM id，用 `setObjectName()` 充当 id。
- **`:root` 变量**：`--name: value` 令牌表 + `var(--name)`（见 §五）。
- **盒子模型**：`box-sizing: border-box` 默认；`rect()` = 外框，`contentRect()` = `rect` 内缩 `border+padding`。

## 三、TS/JS 使用示例

```ts
import { Application, PushButtonWidget, LabelWidget } from "jzwidget";

const app = new Application();

// 1) 全局 author 表（换肤 = 整份替换重设）
const ok = app.setStyleSheet(`
  :root {
    --accent: #3498db;
    --danger: #e74c3c;
    --radius: 8px;
  }

  /* 多类组合：基类 + 变体类（author 层主要风格） */
  .btn       { padding: 7px 14px; border-radius: var(--radius); color: #202020; }
  .btn.primary { background-color: var(--accent); color: #fff; }
  .btn.danger  { background-color: var(--danger); color: #fff; }
  .btn.primary:active { background-color: #2c80c0; }   /* ⚠️ 伪类当前不参与匹配, 暂不生效 */

  /* 类型选择器：UA 默认外观 / reset（只应出现在默认表） */
  JZPushButton { background-color: #e0e0e0; border: 1px solid #808080; }
  JZLabel      { color: #333333; }
`);
if (!ok) throw new Error("样式表语法错误: " + app.styleSheetError());

// 2) 给控件加类（等价 HTML class="btn primary"）
const okBtn = new PushButtonWidget(parent);
okBtn.setStyleClass("btn primary");
okBtn.setText("确定");

// 3) 控件级表：优先级高于全局表，覆盖背景（注意它也作用于本控件的子孙）
const warnBtn = new PushButtonWidget(parent);
warnBtn.setStyleClass("btn danger");
warnBtn.setStyleSheet(".btn.danger { background-color: #c0392b; }");
// 若 setStyleSheet 返回 false，用 warnBtn.styleSheetError() 排查
```

> 在 jzwidget.ts 占位基类里，这些方法是"待 C++ 注册后"可用的真实控件 API；
> 生成器产物（`new MainPanel()` 等 codegen 类）调用方式不变。

## 四、选择器支持矩阵

> 区分两个层次：**解析** = CSS 能被 parse（语法层）；**生效** = 参与规则匹配并真正影响外观。
> 下表的 ✅ 指解析通过 ⚠️ 指"能解析但当前不参与匹配"。

| 写法 | 语法 | 当前状态 |
|------|------|----------|
| 类型选择器 | `JZPushButton` | ✅ |
| 通用选择器 | `*` | ✅ |
| 类选择器 | `.btn` | ✅ |
| 多类复合 | `.btn.primary`（全部须匹配） | ✅ |
| id 选择器 | `#main`（= objectName） | ✅ |
| 逗号组 | `a, b`（展开为多条规则） | ✅ |
| 状态伪类 | `:hover :active :checked :disabled :enabled :focus` | ⚠️ 能解析，但**匹配阶段跳过 → 当前写了不生效** |
| 反选伪类 | `:not(:disabled)`（仅单个已知伪类，`:not(:root)` 不被允许） | ⚠️ 同上（能解析，不参与匹配） |
| 部件寻址 | `::part(arrow)`（可接伪类 `::part(arrow):active`） | ⚠️ 能解析，匹配与消费均未接入 |
| `:root` | 变量作用域 | ✅ |
| 后代/子代组合器 | ` ` 或 `>` | ❌ 当前解析**报错**（计划 M2） |
| 兄弟组合器 | `+` `~` | ❌（计划 M2） |
| 属性选择器 | `[type=text]` | ❌（计划 M4） |
| 伪元素 | `::before` `::after` | ❌ 不支持 |
| `@` 规则 | `@media @import @layer @keyframes` | ❌ 不支持 |

## 五、变量（var()）—— 与 CSS3 最大的语义差异

本引擎的 `var()` 是**解析期文本替换**，而不是 CSS3 的"计算期级联变量"：

1. `--name` **只能在 `:root` 定义**；在别的选择器块里写 `--x: ...` → 解析报错（不是静默忽略）。
2. `var(--name)` 在解析阶段就被替换成 `:root` 里的**字面量**，规则里不保留变量引用。
3. 因此**没有变量继承/作用域覆盖**：像 CSS 里 `.dark-scope { --accent: ... }` 这种局部覆盖令牌，**在本引擎不成立**。
4. 换主题 = **整份替换**（`app.setStyleSheet(整份新 CSS)` → 重新 parse → 整树失效），不能用局部类去覆盖 token。

```css
:root { --accent: #3498db; }
.btn.primary { background-color: var(--accent); }      /* ✅ 解析期替换为 #3498db */
.btn.primary { background-color: var(--accent, #999); } /* ✅ 带 fallback */
.dark { --accent: #fff; }                               /* ❌ 解析报错：自定义属性只能定义在 :root */
```

## 六、与 CSS3 的关键差异清单

1. **无继承**：`color` / `font-*` 不会沿控件树向下传播。每个控件类在自己的 UA 默认里显式声明颜色；"全局改一处"由**整表替换**承担。不支持 `inherit` / `initial`；`currentColor` **尚未实现**（会被当作未知值忽略）。
2. **变量仅在 `:root` + 解析期替换**：见 §五，无级联/作用域。
3. **无布局进 CSS**：`display:flex/grid`、`float`、`width/height`、`position`、`margin:auto` 全部不支持 → 用 `JZLayout` / `setGeometry` / `setVisible`。`margin` 仅作布局留白读取（计划 M3）。
4. **无动画/过渡**：`transition` / `animation` / `@keyframes` 不支持（可后续接 `JZPropertyAnimation`）。
5. **无媒体查询 / `@` 规则 / 伪元素**：`::before/::after` 不支持；`::part()` 是部件寻址（标准语法，
   可解析，但**匹配与消费均未接入**，见 §7.3）。
6. **单位**：仅 `px`（支持小数）。`em` / `%` / `vw` / `vh` / `rem` 不支持；`border-radius:50%`（圆点/圆形）计划 M2 特例 = `min(w,h)/2`。
7. **颜色**：`#RGB #RRGGBB #AARRGGBB` / `rgb()` / `rgba()`（alpha 支持 `0~1` 小数与 `0~255`）/
   `transparent` `none` / 命名色 `white black red green blue yellow cyan magenta gray lightgray darkgray orange`。
   `currentColor` 未实现。解析器不校验属性名与值语义（任意文本都收），
   **真正能否生效由消费层 `JZStyleSet::applyDecl` 决定**。
8. **简写**：`padding` 1~4 值（2 值 = 上下/左右，4 值 = 上/右/下/左）与 `padding-left|top|right|bottom` 分项、
   `border` 简写（`1px solid #ccc`，也接受 `border: none`）**已生效**；
   每边 border（`border-left` 等）未实现。`font-*` / `text-align` / `opacity` / `margin` 均不生效。
9. **特异性/顺序**：与 CSS3 一致（id×100 + (类+伪类)×10 + 类型×1，同值后者胜）；`!important` 跨级覆盖。
10. **属性白名单未接**：当前属性"合法性"交给上层，解析阶段不拦未知属性（写了也不会报错，只是没控件消费）。

## 七、当前落地状态（重要，避免误用）

**解析 → 匹配 → 绘制已打通**（`JZWidget::resolvedStyle()`）。绘制前自动按
`UA 默认 → 全局表 → 控件级表` 解析并缓存；`app.setStyleSheet()` 成功后会自动让所有顶层窗口
**整树**重新解析并重绘 —— 换肤不需要手动刷新，也不需要重建控件。

### 7.1 已接入样式的控件（只有这些会响应 CSS）

| 类 | 响应的内容 |
|---|---|
| `JZWidget` | 背景 / 边框 / 圆角（UA 默认**无背景无边框**，没写样式时与以前完全一致） |
| `JZWindow` | 窗口底色（UA 默认白） |
| `JZPushButton` | 背景 / 边框 / 圆角 / padding / 文字色；**按下 = 背景压暗 ×0.84**（不写死颜色，任意主题通用） |
| `JZLabel` | 背景 / 边框 / 圆角 / padding / 文字色（UA 默认左右 padding 6px、文字 `#333333`） |

其余控件（`JZTextEdit` / `JZListWidget` / `JZComboBox` / `JZTableWidget` …）外观仍是硬编码，
**暂不响应样式**——给它们写 CSS 不会报错，但也不会有任何变化。

### 7.2 已消费的属性（`JZStyleSet::applyDecl`）

`background-color` `background`（色值子集）`color` `border`（简写）`border-color` `border-width`
`border-radius` `padding`（1~4 值）`padding-left|top|right|bottom`。

其余属性解析通过但**不生效**（含 `font-size` / `font-weight` / `font-family` / `text-align` /
`opacity` / `margin` / `border-left` 等）。属性名大小写不敏感。

### 7.3 暂不参与匹配

状态伪类（`:hover :active :checked :disabled :enabled :focus`、`:not(...)`）与 `::part()`：
规则能解析，但匹配阶段直接跳过，**当前写了不生效**。

### 7.4 里程碑

- **M1 解析与级联核心**：✅ 已完成（解析器 + resolve + 缓存 + 整树失效）
- **M2 UA 表化 + 全控件接入**：进行中 —— 当前只接入 §7.1 的 4 个类；UA 默认仍是 C++ `uaStyle()`
  （尚未搬成内置 CSS 文本表）；每边 border、`JZFont` bold/family、组合器均未接
- **M3 状态样式 + margin + opacity + 主题切换**：未开始（主题切换已可用整表替换实现，见 §九）
- **M4（按需）**：属性选择器 / `linear-gradient` / `border-radius:50%` / `text-overflow:ellipsis`

### 7.5 可参考的现成实现

- 换肤 demo：`test/widget/widget`（顶部「主题」按钮，两份 CSS 只差 `:root` 块）
- 单元测试：`test/widget/stylesheet`（target `test_stylesheet`，覆盖解析 / 特异性 / 换肤 / 整树失效）

## 八、排错

- `setStyleSheet(css)` 返回 `false` → 立即读 `styleSheetError()` 拿中文错误（注释未闭合、多余 `}`、空选择器、未知伪类、非 `:root` 自定义属性、组合器、未定义变量等都会明确报错，不静默丢弃）。
- 想验证一段 CSS 是否能被解析，但不想挂到真实控件：可先用 `app.setStyleSheet(css)` 看返回值与错误串。
- 解析器不校验属性值语义：写了 `border-radius: banana` 不会报错，只是没控件消费——这类问题要在 UI 上肉眼确认。
- **语法没错但界面没变化**，按这个顺序排查：
  1. 目标控件类是否在 §7.1 列表里？（不在 → 该控件还没接入样式）
  2. 用了状态伪类或 `::part()`？（§7.3：当前不参与匹配）
  3. 属性是否在 §7.2 清单里？（不在 → 不生效）
  4. 选择器没命中？类型选择器要写**实际类名**（见 §九的坑）；控件忘了 `setStyleClass()`。

## 九、换肤/主题推荐写法

换肤 = **整份替换**：两份 CSS **只有 `:root` 令牌块不同**，组件规则一字不改。

```ts
// 组件规则: 两个主题完全一致, 只吃令牌
const RULES = `
  #main   { background-color: var(--bg); }
  JZLabel { color: var(--fg); }
  JZPushButton {
    background-color: var(--btn);
    border: 1px solid var(--btn-border);
    color: var(--btn-text);
    border-radius: var(--radius);
  }
  .title  { color: var(--accent); }
`;

function applyTheme(app: Application, dark: boolean) {
  const root = dark
    ? `:root { --bg:#161a21; --fg:#e6e9ef; --btn:#2a3140; --btn-border:#4a5364; --btn-text:#e6e9ef; --accent:#4f8cff; --radius:8px; }`
    : `:root { --bg:#ffffff; --fg:#202020; --btn:#e0e0e0; --btn-border:#808080; --btn-text:#202020; --accent:#3498db; --radius:8px; }`;

  if (!app.setStyleSheet(root + RULES))
    throw new Error("样式表语法错误: " + app.styleSheetError());
}
```

调用后框架会自动让所有顶层窗口整树重新解析并重绘，无需手动刷新。

> **坑：类型选择器匹配的是「实际类名」**
> `class MainWindow extends WindowWidget` 的 className 是 `MainWindow`，写 `JZWindow { ... }`
> **匹配不到**（继承链不参与选择器匹配）。顶层窗口请用 `#id`：`win.setObjectName("main")` +
> `#main { ... }`，或写自己的类名。参考 `test/widget/widget`。
