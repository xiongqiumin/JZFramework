# JZStyleSheet 设计 (CSS3 子集, v3.1)

> 2026-09-01 v3: ①绘制分层声明化 —— 盒子层由基类统一绘制 (纯声明), 控件只画内容;
> ②移除 CSS 继承 (伪需求, 我们控制 UA 表); ③清理全部非 CSS3 概念 (:pressed 别名删除)。
> v3.1: **确立多 class 组合为 author 层主要风格** (§2.3), 类型选择器只用于 UA 默认与 reset。
> 目标: 用 CSS 子集统一 JZWidget 外观, 为换肤与 AI 生成主题打基础。

---

## 1. 与 CSS3 的概念对照

**采用的标准概念** (语义与命名对齐):
- 级联来源: **UA 样式表** (内置默认外观) < **author 样式表** (全局 setStyleSheet) <
  **inline style** (`JZWidget::setStyle`, 等价 HTML style="");
- `!important`; 特异性 `(id, 类/伪类, 类型)` 三元组; 同特异性出现顺序后者胜;
- 选择器: 类型 / `.class` (多类复合) / `#id` / `*` / 状态伪类 (`:hover :active :checked :disabled :enabled :focus`) / `:not(伪类)` / 逗号组 / 复合 / 后代(空格)与子代(`>`)组合器 / `::part(name)` (组合控件部件寻址, 伪类可接, 见 §2.4);
  **风格约定: author 层以多 class 组合为主 (基类+变体+状态类), 类型选择器仅用于 UA 默认与 reset (§2.3)**;
- `:root` 变量: `--name: value` 令牌表 + `var(--name)` (parse 期文本替换, 作用域限 :root, 无级联);
- 盒子模型: `content + padding + border`, `box-sizing: border-box` 默认 (rect 即外框);
- 值: `#RGB #RRGGBB #AARRGGBB rgb() rgba()` (支持小数 alpha) 命名色 `transparent` `currentColor`
  `none` / `<n>px` (支持小数) / 简写 (`border`, `padding` 1~4 值, `background` 色值子集)。

**明确不做**: 继承 (inherited properties, 见 §2)、`inherit`/`initial` 关键字、
transition/animation/@keyframes、伪元素、媒体查询、@import、@layer、flex/grid、
`em`/`%` 单位 (border-radius:50% 等)、`margin: auto`、文本溢出系
(white-space/word-break/text-overflow)、行为属性 (cursor/outline/user-select/resize 接受但忽略)。

> 注: CSS 变量 (custom properties) 原列于不做, 经实测审计 (§9) 提升为 M1 子集 ——
> 但仅支持 `:root` 作用域 + parse 期替换, 不做变量级联/继承 (与"无继承"决策一致)。

**非 CSS3 的映射载体 (非新概念, 仅实现载体)**:
- 类型选择器用框架类名 (`JZPushButton`) —— 类名即元素名;
- `objectName` 充当 `#id` —— 控件无 DOM id;
- 布局不进 CSS —— margin 由布局读取是唯一交点, 其余布局仍是 JZLayout。

## 2. 关键决策

### 2.1 样式盒子由控件主动绘制 (JZStylePainter, 定稿)

```
void JZImageLabel::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    JZStylePainter::drawBox(p, style(), rect());        // 盒子层: 背景/边框/圆角 (样式驱动)
    JZRect content = JZStylePainter::contentRect(style(), rect());
    ...                                                 // 内容层: 控件画自己的内容物
}
```

- 控件在 paintEvent 内**主动调用** `JZStylePainter::drawBox(p, style(), rect())`:
  按 styleSet 画 background (填满 rect) → border (rect 内贴) → radius, 全部由样式驱动;
- `style().contentRect(rect())`: rect 内缩 (border-width + padding), 内容物一律基于它定位;
- **绘制边界保证: 绘制流不可能画出控件 rect** —— painter clip = 脏区 ∩ 控件全局 rect
  (JZPainter 构造函数计算, 已实现)。内容可渗入 padding 区但不越过边框
  (等价 CSS overflow:hidden 默认); 画到控件外的内容 (下拉 popup/tooltip) 属于
  弹出层 (独立子窗口) 机制, 不是 paintEvent 的职责;
- 控件 paintEvent 从此只画内容物: PushButton 画文字, Slider 画槽+滑块,
  不再写底色/边框代码;
- 收益: 换肤零代码改动; 外观 100% 来自样式表; 绘制方式显式可见 (不依赖框架隐式插入)。

### 2.1.1 盒子模型与三种矩形

- `rect()` = 控件全矩形 (border-box, 含 border+padding 绘制区) —— 本地坐标原点;
- 内容定位 = `style().contentRect(rect())`;
- rect 之外 (margin 区) 不绘制, 露出父控件背景 (CSS margin 语义)。

### 2.2 不做 CSS 继承 (inherited properties)

`color/font-*` 不沿树传播, 每个控件类的颜色显式声明在 UA 表里。理由:
- 我们控制 UA 表, "改一处全局生效"的场景由**整表替换**承担 (换主题 = 换 author 表),
  不需要继承来省字;
- 省掉: resolve 的父链遍历、inherited 计算值传播、父链级联失效 —— resolve 纯本地,
  缓存失效纯本地;
- 连带删除 `inherit` / `initial` 关键字 (UA 表即 initial); `currentColor` 保留
  (引用本控件自身 color, 与继承无关)。

### 2.3 多类组合为主要风格 (author 层约定)

**author 样式表的主要写法 = 多 class 组合**: 基类 + 变体类 + 状态类, 类型选择器只出现在
UA 表 (默认外观) 与全局 reset —— 与 code_agent / http_server 两份真实 CSS 的组织方式一致。

```css
/* UA 表: 类型选择器, 每类控件的默认外观 (浏览器默认样式的角色) */
JZPushButton { background-color: #e0e0e0; border: 1px solid #808080; border-radius: 4px; }

/* author 表: 多类组合 */
.btn { padding: 7px 14px; border-radius: 8px; color: #202020; }
.btn.primary { background-color: #3498db; color: #fff; }
.btn.ghost   { background-color: transparent; border-color: #d4dbe6; }
.btn.danger:active { background-color: #c0392b; }

/* 动态状态类 (等价网页里 JS toggleClass 的用法): 选中/忙闲/展开等非原生状态 */
.list-item.on { background-color: #eaf3fb; border-color: #3498db; }
.dot.busy { background-color: #f0b429; }
```

- 类是**角色 / 变体 / 自定义状态**的载体; 原生交互状态仍用伪类 (`:hover :active :checked
  :disabled :focus`)。两者特异性同桶 (各 10), 可自由组合 (`.btn.primary:active` = 20);
- API: `setStyleClass("btn primary")` / `addClass` / `removeClass` / `toggleClass("on")`
  (toggle 返回新状态); 类变化 → `styleChanged()`;
- 控件默认**不带**任何类 —— 类由应用/JSON UI 赋予, UA 表用类型选择器兜底,
  与浏览器 UA/author 分层完全一致;
- **类型选择器 = 标签名**: `JZPushButton` 在选择器中的角色等同 HTML 的 `<button>`,
  类名即元素名;
- **Tailwind 式工具类天然兼容**: 工具类 (`.px-4`/`.rounded-lg`) 对引擎而言就是普通 class,
  配合 `:root` 令牌 (`.bg-accent { background-color: var(--accent) }`) 即可实现令牌驱动的
  工具类库; 状态用原生伪类 (不需要 Tailwind 的转义冒号状态类), 响应式前缀/`em`/`%` 不做;
- JZUiBuilder 的 JSON schema 直接携带 `"class": "btn primary"` (或工具类串) ——
  AI 生成界面即多类组合, 换肤/变体只是改类名。

### 2.4 ::part 组合控件部件寻址 (CSS Scoping 标准机制)

组合控件 (JZComboBox 等) 是 Web Component 语义: **JS 由 C++ 实现, 样式交给 CSS,
内部实现自由** —— 子部件可能真实创建子 Widget, 也可能 owner-draw 画出, 外部不可见。
部件寻址用 CSS Scoping 的标准 `::part()` 语法 (非 Qt 私有 subcontrol 扩展):

```css
JZComboBox::part(arrow)          { color: #555; }
JZComboBox::part(arrow):active   { color: var(--accent); }      /* 伪类可接在 part 后 */
JZComboBox::part(popup)          { border: 1px solid var(--border); }
JZComboBox::part(option):checked { background: var(--accent-dim); }
```

- **part 契约与实现解耦**: 内部真实子控件 (`setPartName("arrow")` 标注归属) 或
  owner-draw (`styleFor("arrow")` 查询) 皆可消费同一份 `::part(arrow)` 规则,
  实现切换时 CSS 零改动;
- **popup**: 弹出列表要画到控件 rect 外 → 独立无框弹窗, 携带 owner 上下文,
  `::part(popup)` 经全局样式表命中;
- part 名进语义树 (`parts: ["display","arrow","popup"]`), AI 按部件寻址;
- 与"排除伪元素"不冲突: 排除的是 `::before/::after` 生成内容; `::part` 是部件寻址,
  且为标准语法。

控件 part 契约示例 (JZComboBox): `display` 值显示 / `arrow` 下拉箭头 /
`popup` 弹出列表 / `option` 选项。

## 3. 架构

```
CSS 文本 ──parse──► JZStyleRule[] (值已解析校验, 不可变)
                        │
控件 styleSet()          │
  1. 收集: UA 表 + author 表中匹配 (类型/#id/*/伪类/组合器) 的规则
     + 自身 inline 声明块 (特异性视作最高, !important 再提)
  2. 特异性排序, 顺序合并 → 类型化 JZStyleSet (缓存)
                        ▼
paintBox() 盒子层 (基类)  →  paintEvent() 内容层 (控件)
```

级联: `UA < author < inline`, `!important` 跨级覆盖, 特异性三元组, 同值后者胜。
无继承: resolve 纯本地匹配, 不遍历父链。

## 4. 属性与值

| 属性 | 值 | 消费方 | 阶段 |
|---|---|---|---|
| background-color / background(色值子集) | 颜色, none, transparent | paintBox 填充 | M1 |
| border (简写) | `1px solid #ccc`; style 仅 solid/none | paintBox 描边 | M1 |
| border-left/-right/-top/-bottom | 同上, 每边独立 (分隔线高频用法) | paintBox 四边分别描边 | M2 |
| border-radius | px; `50%` 特例 = min(w,h)/2 (圆点/圆形) | paintBox 圆角 | M1 (50% M2) |
| padding (+-left/-top/-right/-bottom) | px, **1~4 值简写** | contentRect 内缩 | M1 |
| color | 颜色, currentColor 场景 | 内容层文本画笔 | M1 |
| text-align | left/center/right | Label/TextEdit | M2 |
| line-height | 倍数 (1.6) 或 px | TextEdit 多行步进 | M2 |
| opacity | 0..1 小数 | 控件全部绘制任务的 alpha | M3 |
| font-family | `"Microsoft YaHei", sans-serif` 回退列表 | 后端字体族 (回退结构先就位) | M2 |
| font-size | px (支持小数, 内部取整) | JZFont | M1 |
| font-weight | normal, bold, 100..900 (≥600 bold) | JZFont bold 位 | M1 |
| font-style | normal / italic | JZFont style 位 (与 bold 同一字段族) | M2 |
| margin (+分项) | px (不支持 auto) | 布局读取 | M3 |
| box-sizing | border-box (默认) / content-box | contentRect 计算 | M2 |

## 5. 数据模型

```cpp
struct JZStyleSelector {
    std::string className;              // 类型: 空=不限, "*"=全部
    std::vector<std::string> classes;   // .class 复合 ("btn primary" 多类全须匹配)
    std::string objectId;               // 空=不限
    std::vector<std::string> pseudos;   // hover/active/checked/disabled/enabled/focus
                                        // :not(x) 记录为反向匹配项
    std::string part;                   // ::part(name) 部件寻址 (空=不限)
    std::string combinator;             // "" | " " | ">" (M2)
    bool match(const JZWidget* w, unsigned state) const;
    bool matchPart(const JZWidget* w, const std::string& partName, unsigned state) const;
    int specificity() const;
};

struct JZStyleRule {
    std::vector<JZStyleSelector> selectors;
    std::vector<JZStyleDecl> decls;     // 已解析: Prop + 类型化值 + important 位
};

class JZStyleSheet {
public:
    bool parse(const std::string& css);           // 失败 false + errorString()
    JZStyleSet resolve(const JZWidget* w) const;  // 纯本地匹配, 无父链
    // 部件查询: 匹配 "类型::part(name)" 规则 (组合控件 owner-draw 实现用)
    JZStyleSet resolvePart(const JZWidget* w, const std::string& partName) const;
private:
    std::vector<JZStyleRule> m_rules;             // author 表
    // UA 表: 内置 css 静态解析一次 (进程级)
};

// inline style (等价 style="" 属性)
void JZWidget::setStyle(const std::string& declarations);

// JZStyleSet: Prop 枚举 + has()/color(p,def)/length(p,def)/flag(p,def)
//   + 具名便捷 backgroundColor()/textColor()/borderWidth()/radius()/paddingLeft()...
//   + contentRect(w) (border+padding 内缩)
```

## 6. 控件集成

**前置件**:
1. `JZWidget::setObjectName/objectName()` + 多类 API: `setStyleClass("btn primary")` /
   `addClass` / `removeClass` / `toggleClass` / `hasClass` / `styleClass()`
   (类以空格分隔, 同 HTML class 属性; 变化触发 styleChanged());
2. `enum StyleState { Hover=1, Pressed=2, Checked=4, Disabled=8, Focus=16 }` +
   `virtual unsigned styleState() const`; 状态变化 → `styleChanged()` (清缓存+update());
   hover 依赖输入链路 (WndProc 恢复后补 enter/leave);
3. `JZFont` 加 bold/family (Text 任务透传, GdiFontEngine/replay 用 FontStyleBold —— 唯一渲染侧改动);
4. **部件 API** (§2.4): `JZWidget::setPartName/partName()` (子控件标注归属) +
   组合控件查询 `JZStyleSet styleFor(const std::string& part) const`
   (= style() ∪ 样式表 `::part(name)` 规则); 语义树输出 parts 列表。

**内置 UA 样式表** (现状硬编码整体搬入):
```css
JZWindow { background-color: #ffffff; color: #202020; font-size: 14px; }
JZPushButton { background-color: #e0e0e0; border: 1px solid #808080;
               border-radius: 4px; padding: 4px; color: #202020; font-size: 14px; }
JZPushButton:active { background-color: #c0c0c0; }
JZLabel { color: #333333; font-size: 14px; }
...
```

**缓存**: styleSet() 缓存, 失效键 = (表版本, styleState 位); 表替换整树失效;
inline setStyle 只失效自身。

**控件改造** (M2): paintEvent 删除盒子绘制代码, 只画内容; 内容坐标基于 contentRect()。

## 7. 与现有机制 / AI 方向的衔接

- 渲染: paintBox 产出仍是 JZDrawTask (fill + round-rect), GDI+ 后端除 bold/font-family 零改动;
- 布局 (M3): JZBoxLayout/JZGridLayout 摆放子项读取 margin 留白 (CSS 盒模型唯一交点);
- AI-Native: 界面 = JZUiBuilder JSON (结构) + JZStyleSheet CSS (外观), 都是文本协议,
  LLM 可直接生成; `build_theme` agent tool 水到渠成。

## 8. 里程碑

| 里程碑 | 内容 | 验收 |
|---|---|---|
| M1 解析与级联核心 | 解析器 (!important/currentColor/简写/多值 padding/小数) + 类选择器 + ::part() + :root 变量+var() + :not(简单伪类) + 模型 + objectName/styleClass/styleState/partName + setStyleSheet/setStyle + JZStylePainter 骨架 | 单测断言 resolve (特异性/!important/变量替换); 无视觉变化 |
| M2 UA 化 + 全控件接入 | 硬编码搬入 UA 表; 盒子层接管; 控件改内容绘制; 每边 border; JZFont bold/family + 后端; 组合器 | 截图与现状一致; 全局表整体换色生效 |
| M3 状态样式 + margin | :hover/:active/:checked/:disabled 联动; 布局读 margin; opacity; 深色主题 demo | 按下变深; 深浅主题一键切换 |
| M4 按需 | 属性选择器 / linear-gradient / border-radius:50% / text-overflow:ellipsis / 子树样式表(标注为自有扩展) | 按需 |

工作量: M1 ~1.5 天, M2 ~1 天, M3 ~1 天, M4 待定。

## 9. 实测审计: binary/code_agent/web/css/style.css

以项目内真实的 code_agent Web 界面样式 (416 行, 现代写法: 主题令牌 + 类选择器 + flex)
为样本, 检验子集覆盖度。元素映射: `.app`→JZWindow+HBox, `.sidebar`→JZWidget(固定宽),
`#fileList`→JZListWidget, `.btn`→JZPushButton, `textarea`→JZTextEdit, `.modal`→JZDialog,
`.bubble`→JZChatPanel(规划中)。

| 类别 | 该文件的用法 | 结论 |
|---|---|---|
| 变量 | `:root { --bg: ... }` + 全部颜色走 `var()` | **新增支持** (M1, parse 期替换) |
| 选择器 | `.btn` / `.dot.busy` / `.btn.primary` / `.file-list li:hover` / `.modal.hidden` / `.btn:hover:not(:disabled)` | `.class`+多类 **新增 (M1)**; `:not` **新增 (M1)**; 后代组合器 M2 |
| 盒模型 | `border-right/bottom/top` 分隔线 ×6; `padding: 14px 16px 8px` 多值 | 每边 border **提级 M2**; 多值 padding M1 |
| 视觉 | background/rgba 小数 alpha/font-size 12.5px/font-weight 600/border-radius | M1 支持 |
| 文本 | text-align/line-height/ellipsis/word-break | 前两者 M2; 溢出系不做 |
| 动画 | @keyframes pulse/blink + transition | 不做 (后续可接 JZPropertyAnimation) |
| 布局 | display:flex 系 / width/height/position:fixed/margin:0 auto | 不做 → JZLayout / 几何 API / JZDialog |
| 行为 | cursor/outline/user-select/resize/list-style | 接受并忽略 |

覆盖度: 按"视觉呈现"口径约 **85%** 直接覆盖; 其余为布局/动画/行为, 属于框架其它机制的职责。
该文件删去 flex 系与动画声明后, 其余规则基本可被 M1+M2 子集原样解析。

### 9.2 http_server/style.css (ugreen-server, 工具页风格: 表单+日志+对话框, 1472 行)

样本特征: 无变量、无动画、大量 inline style、传统布局 (float/calc) + 表单控件。
元素映射: `.container`→JZWindow, `.config-card/.status/.note`→JZWidget+class, `select`→JZComboBox,
`input[type=checkbox]`→JZCheckbox, `input[type=text]`→JZTextEdit, `button`→JZPushButton,
`#log`→JZTextEdit(滚动+等宽), `.dialog`→JZDialog, `.progress-bar`→JZProgressBar, `label`→JZLabel。

| 类别 | 该文件的用法 | 结论 |
|---|---|---|
| 选择器 | `.class` 组、`h1,h2,h3,h4` 类型组、`#log`、`:hover/:focus/:disabled`、`button:hover:not(:disabled)`、`.dir-tree .folder` 后代、`canvas#wave` 复合 | 全覆盖 (M1+M2) |
| 需新增 | `font-style: italic` (.ini-comment) | **新增 M2** (JZFont style 位, 与 bold 同批) |
| 需新增 | `border-radius: 50%` (.tooltip-icon 圆点) | **新增 M2** 特例: min(w,h)/2 |
| 盒模型 | border 多值/每边、padding 多值、margin 多值、box-sizing | M1/M2 已覆盖 |
| 文本 | color/font-size/weight/text-align/line-height | M1/M2 已覆盖 |
| **不支持-布局** | display:flex/inline/none、float、`calc(50% - 10px)`、width/height/min-*/max-*、position+top/left、z-index、`:nth-child(2n)` | → JZLayout / 几何 API / setVisible; nth-child 场景由布局承担 |
| **不支持-效果** | box-shadow (卡片阴影, 高频)、text-shadow (字幕发光)、transform、transition、linear-gradient (进度条填充 ×3) | M4 候选: gradient (进度条) / box-shadow; 其余排除 |
| 忽略项 | cursor/outline/user-select/resize/text-decoration/aspect-ratio/accent-color/`font-variant-numeric` | 接受并忽略 |
| inline style | index.html 大量 `style="..."` (width/padding/border-right:none/background) | 即 setStyle (M1), 内容全部可解析 |

**验收结论**: 该文件没有引入任何新的结构性概念。补上 font-style 与 border-radius:50% 两个
小项后, 其**视觉规则 100% 落在子集内**; 不覆盖部分全部是布局 (display/float/calc/尺寸)、
窗口行为 (position/visibility/z-index → setVisible/JZDialog) 与装饰效果 (shadow/transform/
transition)。样式表现实用法检验通过 —— "覆盖这类文件"作为 M2 的验收标准成立。
