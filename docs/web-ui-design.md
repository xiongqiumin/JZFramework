# Web UI 设计：统一视觉层（jz-ui）

> 2026-11-06 成文。**状态：P0 已落地（`web/ui/{tokens,base,components}.css` + `demo.html` + `jz-ui.js`），
> `agent_cli` 控制台与 `agent_cli_server` 管理台已引用；页面内部 class 迁移到 `.jz-*` 未做（见 §9 P1）**。
> 落地后本文继续作为"视觉层"的唯一文档，各页面的业务结构（谁调用什么接口）不写在这里。

---

## 1. 背景与约束

现在有三处 Web 界面，样式各写各的：

| 界面 | 实现 | 可维护性 |
| --- | --- | --- |
| `agent_cli` 控制台 | `binary/agent_cli/webui/{index.html,agent.html,tasks.html,app.js,app.css}`，构建时拷到 `<exeDir>/webui/` | 与 server 管理台**共用同一份**前端（2026-11-11 合并） |
| `agent_cli_server` 管理台 | 同一份源 `binary/agent_cli/webui/`，构建时拷到 `<exeDir>/server_web/`，运行时读文件 | 改样式不用重编译 |
| `JZSimulator` | `JZSimulator/http/index.html` | 同上，第三套 |

四条硬约束（决定选型，不能妥协）：

| # | 约束 | 后果 |
| --- | --- | --- |
| 1 | **离线 / 内网部署**（node 侧根本不联网） | 禁止 CDN；所有资源必须随包分发 |
| 2 | **单 exe 发布** | 禁止需要 node/npm 的构建链（Tailwind CLI、Vite、TS 编译都算） |
| 3 | **宿主不同**：静态目录 / C++ 内嵌字符串 | 视觉层必须是可复制的纯文件，且能被"内嵌页"引用 |
| 4 | **三端**：手机 / 平板 / 电脑 | 布局不能假设鼠标、不能写死列宽 |

规模事实：**3 个页面、10 类组件**。这个规模决定了"引入一个完整组件库"是负收益（见 §3）。

---

## 2. 需求：要沉淀的组件（从现有页面倒推）

| 类别 | 现有东西（现在的 class） | 收敛为 |
| --- | --- | --- |
| 骨架 | `.app` `.body` `.side` `.chat` `.splitter` | `.jz-shell` / `.jz-split` |
| 顶栏 | `.topbar` `.brand` `.logo` `.status` `.dot` | `.jz-topbar` |
| 按钮 | `.primary` `.ghost` `.tiny` | `.jz-btn` + 变体 + 尺寸 |
| 徽标 | `.badge` `.task-badge` `.term-badge` | `.jz-badge` + 语义色 |
| 卡片 | `.task-card` `.card`(server) | `.jz-card` |
| 表格 | server 管理台的两张表 | `.jz-table` / `.jz-table-wrap` |
| 表单 | `input` / `textarea` / `.composer-field` | `.jz-field` / `.jz-input` |
| 日志块 | `pre.log`(server) `.term-log` `.proc-body` | `.jz-log` |
| 时间线 | `.proc` `.proc-head` `.proc-dot` `.proc-tag` | `.jz-step` |
| 空状态 | `.side-empty` `.empty`(server) | `.jz-empty` |
| 反馈 | 无（server 现在没有 toast） | `.jz-toast`（JS） |
| 对话框 | 无 | 原生 `<dialog>`（不自研） |

---

## 3. 选型：引入通用库，还是自己写

### 3.1 候选对比

| 方案 | 无构建 | 离线/内网 | 体积 | 可定制性 | 与现有 class 冲突 | 结论 |
| --- | --- | --- | --- | --- | --- | --- |
| **Shoelace / Web Awesome**（原生 Web Components + ESM） | ✅ 复制文件即可 | ✅ | 中（全量 ~300KB，可按组件引） | 中（靠 CSS parts / 变量，深度定制要穿透 Shadow DOM） | 低（`sl-` 前缀） | **候选**，需要复杂组件时才上 |
| **Bootstrap 5** | ⚠ 要带 bundle.js（含 Popper） | ✅ | 大（CSS ~230KB） | 低（改起来像在和它对抗） | 中（`.btn/.card/.badge` 会撞） | ❌ 视觉不可控 |
| **Bulma** | ✅ 纯 CSS | ✅ | 中（~200KB） | 中 | 中 | ❌ 为 3 个页面背 200KB |
| **Pico.css**（classless） | ✅ 极小 40KB | ✅ | 小 | 低（就是"默认好看"） | 低 | ❌ 撑不起控制台（表格/分栏/日志） |
| **Tailwind** | ❌ 要 CLI 构建；CDN 版是运行时 JIT，不许上生产 | ❌（CDN 版） | 小（产物） | 高 | 低 | ❌ 违反约束 2 |
| **Open Props + 自研** | ✅ | ✅ | 小（~10KB 变量） | 高 | 低 | 备选（只借 tokens 思路） |
| **自研 tokens + components.css + 极小 JS** | ✅ | ✅ | **最小（~15KB）** | **完全** | 无（新前缀） | ✅ **推荐** |

### 3.2 结论与判据

**采取"薄自研"**：自己写 tokens / base / components（约 600 行 CSS + 200 行 JS），
不引入框架、不引入完整组件库。理由：

1. 组件面只有 10 类、全是"静态外观 + 少量交互"，没有任何一个需要虚拟滚动、日期选择器、可搜索下拉；
2. 三个宿主里有一个是 **C++ 内嵌字符串**，任何依赖"打包器/模块解析"的库都要额外做一层搬运；
3. 现有 `app.js` 是 43KB 原生 JS，引入框架意味着重写它（成本远高于写 600 行 CSS）；
4. 视觉要能一句话改（比如主色 `--accent`），库的定制往往是"覆盖再覆盖"。

**什么时候该改主意**（写进文档，免得以后反复讨论）：

- 需要 ≥3 个**复杂交互**组件（可搜索下拉 / 树 / 日历 / 虚拟滚动表格）→ 引入 **Shoelace**，只引需要的组件，页面结构不动（因为我们的 class 与它是两套命名，可以并存）；
- 需要多主题 + 无障碍审计（WCAG AA）→ 引入 Shoelace（它的 a11y 是现成的）；
- 页面数 >10 → 自研的边际成本开始反超。

**复杂交互的例外（先不引库，直接用平台能力）**：

- 对话框用原生 `<dialog>`（`showModal()`，自带焦点陷阱与 Esc）：外观是 `.jz-dialog`，
  打开用 `JzUI.dialog()` / `JzUI.confirm()`（Promise 化，见 `jz-ui.js`）；
- 轻量浮层用 **Popover API** + CSS anchor positioning（Chrome/Edge 已稳定）；
- 提示用 `title` 或 `.jz-toast`，不引 tooltip 库。

---

## 4. 架构分层

```
tokens.css      只放变量(--jz-*)，不含选择器
   ↑
base.css        reset + 排版 + 布局原语(.jz-shell/.jz-split/.jz-row/.jz-stack/.jz-grid)
   ↑
components.css  组件外观(.jz-btn/.jz-card/.jz-table/.jz-badge/.jz-log/.jz-step...)
   ↑
app.css         页面特例(只写"这套组件拼不成"的东西)
```

规则：

1. **组件只认变量**，不写死颜色/圆角/间距；页面不直接写颜色。
2. 命名空间统一：变量 `--jz-*`、类 `.jz-*`、自定义元素 `<jz-*>`、data 属性 `data-jz-*`。
3. **不改 `agent_cli` 现有变量名**（`--bg/--panel/--text/--muted/--accent/--ok/--fail/--border`），
   迁移期在 `tokens.css` 里做一层映射，页面改完后删掉映射：

```css
:root{
  --jz-bg:#f3f5f8;      --jz-panel:#fff;     --jz-border:#e2e5ea;
  --jz-text:#1f2328;    --jz-muted:#6b7280;  --jz-accent:#2f6fed;
  --jz-ok:#1a7f37;      --jz-fail:#cf222e;
  /* 迁移期兼容：老页面继续用旧名 */
  --bg:var(--jz-bg); --panel:var(--jz-panel); --border:var(--jz-border);
  --text:var(--jz-text); --muted:var(--jz-muted); --accent:var(--jz-accent);
  --ok:var(--jz-ok); --fail:var(--jz-fail);
}
```

4. 主题：**只做浅色一种显示**（2026-11-06 定）。不做 `prefers-color-scheme` 分支、不做 `[data-theme]`，
   `tokens.css` 里也没有暗色变量位 —— 少一半验收面，等真有需要再加（加的时候只改 tokens，不动组件）。

尺度（4px 基准，全部来自变量）：

| 变量 | 值 | 用途 |
| --- | --- | --- |
| `--jz-sp-1..6` | 4/8/12/16/24/32 | 内外边距、gap |
| `--jz-r-1..3` | 6/10/14 | 圆角（按钮/卡片/大容器） |
| `--jz-fs-1..4` | 12/13/14/20 | 辅助/正文/标题/标题大 |
| `--jz-shadow-1` | `0 1px 2px rgba(31,35,40,.06)` | 卡片/顶栏 |
| `--jz-tap` | `44px` | 触控最小命中区 |

---

## 5. 目录与分发

```
web/ui/
├── tokens.css
├── base.css
├── components.css
├── jz-ui.js          # 经典 script(非 module)：toast / 日志自动贴底
├── demo.html         # 组件总览(改样式时的肉眼验收页；随包走, 现场也能打开看)
└── README.md         # 怎么加一个组件、怎么改主题
```

分发一份源、三处消费：

| 宿主 | 怎么拿到 |
| --- | --- |
| `agent_cli` | CMake POST_BUILD：`copy_directory <root>/web/ui → <exeDir>/webui/ui`；页面里 `<link href="/ui/components.css">` |
| `agent_cli_server` | 页面**不在 C++ 里**：与 cli 同一份源 `binary/agent_cli/webui/` → POST_BUILD 拷到 `<exeDir>/server_web/`，`web/ui` 再拷进 `server_web/ui`；`/` 读文件返回，`/app.css` `/app.js` `/ui/*` 走静态路由。**目录缺失 = 500 + 明确提示**（不做内嵌兜底） |
| `JZSimulator` | qrc 用 `alias` 直接指到 `web/ui/tokens.css`，`css/style.css` 里 `@import`，不复制副本 |

⚠ 前端源码只有一份（`binary/agent_cli/webui/`），但**运行目录分开**：server 用 `<exeDir>/server_web/`，cli 用 `<exeDir>/webui/` —— 两个 exe 同目录时才不会互相覆盖。

缓存：页面引用统一带版本 `?v=N`（沿用现在的做法），升级后改 N；`JZHttpStaticFiles`
已有 1 小时 `max-age`，所以**必须**靠 `?v=` 失效，否则改了样式现场看不到。

---

## 6. 响应式规范（三端）

| 断点 | 目标 | 布局 |
| --- | --- | --- |
| `<640px` | 手机 | 单列；侧栏变成抽屉/折叠块；表格转卡片；操作按钮进底部工具条 |
| `640–1024px` | 平板 | 两列（侧栏 240px + 主区）；允许横滑表格 |
| `>1024px` | 桌面 | 侧栏固定 + 主区自适应（沿用现在 `max-width:1280px` 居中） |

六条硬规则：

1. 尺寸用 `clamp()`：`font-size:clamp(13px,1.6vw,15px)`、`padding:clamp(8px,1.2vw,16px)`；不写死列数。
2. 栅格用 `grid-template-columns:repeat(auto-fit,minmax(240px,1fr))`；需要独立适配的卡片用 `@container`。
3. **表格是手机端唯一难点**，全站统一一种策略：`<640px` 时用 `data-label` 把每行变卡片
   （`td::before{content:attr(data-label)}`）；桌面/平板用 `.jz-table-wrap{overflow:auto}` + 首列 sticky。
4. 触控：可点元素 `min-height:var(--jz-tap)`；hover 效果包在 `@media (hover:hover)` 里（否则手机上"粘住"）。
5. 高度用 `100dvh`，不用 `100vh`（移动端地址栏）。
6. `prefers-reduced-motion` 时关闭过渡。

---

## 7. 组件规范（每类只定义一次）

| 组件 | 结构 | 变体 / 状态 |
| --- | --- | --- |
| 按钮 | `<button class="jz-btn">` | `--primary` `--ghost` `--danger`；`--sm`；`:disabled` |
| 徽标 | `<span class="jz-badge">` | `--ok` `--fail` `--warn` `--info` `--idle` |
| 卡片 | `.jz-card > .jz-card__hd/.jz-card__bd/.jz-card__ft` | `--flat`（无阴影，用于嵌套） |
| 表格 | `.jz-table-wrap > table.jz-table` | 行选中 `tr.is-sel`；手机卡片化（§6.3） |
| 表单 | `.jz-field > label + .jz-input` | `.jz-input--err`；`textarea` 同色系 |
| 日志 | `<pre class="jz-log">` | 等宽、`overflow:auto`、可选 `data-autoscroll`（贴底） |
| 时间线 | `.jz-step > .jz-step__dot + .jz-step__tag + .jz-step__body` | `--ok` `--fail` `--run`（运行中脉冲，尊重 reduced-motion） |
| 空状态 | `.jz-empty` | 一句话说明 + 可选一个按钮（**不要只放图标**） |
| 顶栏 | `.jz-topbar > .jz-brand + .jz-tools` | 手机端 `--sticky` |
| Toast | `jz-ui.js` 注入 `#jz-toasts`（`aria-live="polite"`） | `ok/fail/warn`；3s 自动消失；同时最多 3 条 |

**不做**：自研 modal / tooltip / 下拉菜单（用 `<dialog>`、`title`、Popover）；
不做"表格分页组件"（现在三个页面都不需要，等真需要再说）。

---

## 8. JS 约定

- **ES module，无构建**：`<script type="module" src="/ui/jz-ui.js">`；不用 bundle、不用转译，
  代码里只用 ES2020 以内（`?.`/`??` 可用）。
- **渐进增强**：布局与配色在**没有 JS**时也必须正确；JS 只做 toast、dialog 增强、表格排序、
  自动贴底这类"锦上添花"。
- **无全局污染**：只导出 `JzUI.toast()` 等少数函数 + 注册 `jz-*` 自定义元素；不挂 `window.xxx`。
- **数据驱动**：行为靠 `data-*`（`data-sortable`、`data-autoscroll`），不靠给每个元素加 `onclick`。
- **不引第三方**：不引 htmx（我们已有 SSE）、不引 jQuery、不引图标库（图标用内联 SVG，
  统一放 `web/ui/icons.svg` 作为 `<symbol>` sprite）。

---

## 9. 迁移路径

| 阶段 | 内容 | 验收 |
| --- | --- | --- |
| **P0** | `web/ui/{tokens,base,components}.css` + `demo.html` | demo 页在 375/768/1440 三个宽度下无横向滚动、无重叠；按钮命中区 ≥44px |
| **P1** 🚧 部分 | `agent_cli` 控制台：已引用 tokens/base/components（删掉自己的 `:root`，颜色统一）；**页面内部 class 仍是 `.composer/.bubble/.proc`**，由 `app.js` 拼装 | 颜色/圆角/字号与 jz-ui 一致；`.jz-*` 迁移待做（见 §10 第 7 条） |
| **P2** ✅ 完成 | `agent_cli_server` 管理台：页面拆成 `server/web/{index.html,app.css,app.js}`（**不再内嵌 C++ 字符串**），整体用 `.jz-*`（topbar/kpi 卡/table/badge/btn/log，表格带 `data-label` 手机转卡片）；`/` 读文件返回，`/app.css` `/app.js` `/ui/*` 静态路由 | 改样式只改文件+刷新，不重编译；缺 `server_web/` 时 `/` 回 500 并说明原因 |
| **P3** ✅ 完成 | `JZSimulator`：`css/style.css` 改为 `@import "../ui/tokens.css"` + 全套 token 化（浅色）；qrc 用 `alias` 指到 `web/ui/tokens.css`（不在本目录放副本）；驱动页共用同一份 style.css 自动跟随 | 主界面与 `drivers/*.html` 统一浅色 |
| **P4**（可选） | `jz-ui.js` 的 toast / 表格排序 / 自动贴底 | 无 JS 时功能降级但布局不变 |

依赖：P1 依赖 P0；P2 依赖 P1（同一套变量才能看出差异）；P3 只依赖 P0。

---

## 10. 风险与未决

1. **双份样式并存期**：P1 做完才能删旧的 `style.css` 段落，否则中间态最难查；迁移期用变量映射过渡（§4.3）。
2. **server 不再是单文件**：管理台依赖 `server_web/`（index.html + app.css + app.js + ui/）。发布时**必须整目录带上**；
   缺目录的表现是 `/` 返回 500 + "读不到管理台页面: .../server_web/index.html"，不是空白页（刻意不做内嵌兜底，
   否则"改文件生效"和"内嵌副本"会长期不一致）。
3. ~~暗色主题~~ **已定：不做**，只做浅色一种显示（§4.4）。
4. ~~主色是否统一~~ **已定：统一到 `#2f6fed`**（`--jz-accent`）。管理台原先的 GitHub 蓝 `#0969da`
   已经在 `tokens.css` 里被 `--blu: var(--jz-accent)` 收编，页面不用改代码。
5. **移动端"表格转卡片"要不要 P0 就做**：建议做，否则 P1 之后还要回头改表格的 DOM（要加 `data-label`）。
6. **图标**：现在页面用 emoji（`⚙`）和文字；统一 sprite 之后要检查是否仍有 emoji 残留（不同系统渲染差异大）。
7. **`agent_cli` 控制台的 class 迁移（唯一剩余）**：`.composer/.composer-field/.bubble/.msg/.proc*/.task-card/.badge/.term-*`
   都在 `binary/agent_cli/webui/app.js` 里以模板字符串拼装，且 `.proc` 是**两层结构**（`proc-head` 包 dot/tag/name）
   而 `.jz-step` 是平铺（dot/tag/body）—— 迁移必须同时改 HTML 结构与更新逻辑（流式追加 `proc-body`），
   不是纯改名。建议单独一轮做，并用控制台实跑一个任务回归（thinking/tool_call/verify 三类行都要看）。
