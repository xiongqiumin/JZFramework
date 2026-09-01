# JZFramework 原生 AI 交互设计 (AI-Native UI Framework)

> 2026-09-01 起稿。目标: 让 AI 成为框架的一等公民 ——
> AI 可以**看**(语义树)、**动**(驱动 API)、**听**(事件流)、**说**(流式控件)、**建**(声明式 UI)。

---

## 0. 现状盘点 (设计依据)

已有能力 (直接复用, 不重复造):

| 模块 | 已有能力 | 在本设计中的角色 |
|---|---|---|
| JZAgent | OpenAI 兼容 LLM 客户端 + SSE 流式 + 完整 tool-calling 循环 (maxRounds/护栏) | **AI 大脑**, 无需再写 LLM 客户端 |
| JZMcpServer | MCP tools 子集 (JSON-RPC, HTTP + WebSocket 传输, 异步 tool + 信号回送) | AI 与 UI 之间的**标准协议出口** |
| JZJSEngine | QuickJS + jsbind + Promise/异步 + fetch | 胶水层 / 用户脚本 agent |
| JZNet | WebSocket client/server、HTTP client (keep-alive/TLS, Agent 的底座) | 事件流推送通道 |
| JZObject | 信号槽 (跨线程 Auto→Queued 投递事件队列) + JZVariant | **线程桥的现成机制** |
| JZWidget | 保留模式控件树 + paintEvent + postEvent 失效重绘 (JZWin32Thread 单线程 UI) | 被观测/被驱动的目标 |
| JZJson | jsoncpp DOM + 流式序列化 | 所有接口的数据格式 |

核心缺口 (全部在 UI 侧):
1. 控件没有语义信息 (role/name/value/actions) → AI 只能看像素, 无法理解界面;
2. 没有程序化驱动 API → AI 无法读状态/调动作;
3. 没有声明式 UI → AI 无法生成界面 (LLM 输出 C++ 不可行);
4. 没有流式内容控件 → LLM token 流无处安放;
5. JZObject 无按名反射 → 工具导出全手工 (McpTool/AgentTool/jsbind 三套各写一遍);
6. JZMcpServer 缺 initialize 握手 / resources / prompts / stdio → 主流 MCP 客户端连不上。

---

## 1. 定位与三个角色

```
┌─────────────────────────────────────────────────────┐
│                    AI (任何线程)                      │
│   JZAgent(LLM) / 外部 MCP 客户端 / QuickJS 脚本       │
└──────┬──────────────┬──────────────┬────────────────┘
       │ MCP tools     │ WebSocket     │ C++ API
       ▼               ▼ (事件流推送)   ▼
┌─────────────────────────────────────────────────────┐
│  L3 协议层: JZUiMcpBridge (tools+resources) / WS 推送 │
├─────────────────────────────────────────────────────┤
│  L2 驱动层: JZUiDriver                               │
│    find(selector) / read / act / observe             │
│    (全部经 postEvent 投递到 UI 线程执行, 结果信号回送)  │
├─────────────────────────────────────────────────────┤
│  L1 语义层: JZWidget::aiRole/aiName/aiValue/aiActions│
│             dumpSemanticTree()  ← AI 的"眼睛"        │
├─────────────────────────────────────────────────────┤
│  JZWidget 控件树 (UI 线程: JZWin32Thread 消息循环)     │
│   + L4 生成层: JZUiBuilder::fromJson (声明式 UI)      │
│   + L5 会话层: JZChatPanel / JZMarkdownView (流式)    │
└─────────────────────────────────────────────────────┘
```

**AI 的三个角色** (决定三组 API):

1. **AI as Driver (AI 操作 UI)** — 界面是 agent 的 tool。
   agent 读语义树 → 推理 → 调动作 → 观察变化。典型: 自动化测试、无障碍代理、运维 agent。
2. **AI as Builder (AI 生成 UI)** — 界面是 agent 的输出。
   LLM 输出声明式 JSON → JZUiBuilder 构建控件树 → 热替换。LLM 擅长 JSON, 不擅长 C++。
3. **AI as Companion (UI 内嵌 AI)** — AI 是界面的内容/功能。
   聊天面板、流式 markdown、语义搜索。JZAgent 已有闭环, 缺的只是 UI 控件。

---

## 2. 分层设计

### L1 语义层 — AI 的眼睛 (地基, 最优先)

JZWidget 增加四个虚函数 + 一个导出方法:

```cpp
class JZWidget {
public:
    // 语义接口 (默认实现给出合理值, 各控件按需重写)
    virtual std::string aiRole() const;      // "window"/"button"/"slider"/"list"/"text"...
    virtual std::string aiName() const;      // 人类可读名 (默认: objectName 或文本内容)
    virtual JZVariant aiValue() const;       // 当前值: 文本/选中/进度值...
    virtual std::vector<JZAiAction> aiActions() const;  // 支持的动作清单

    // 整棵语义树导出 (带几何: scale 后的逻辑 rect, 供多模态模型对准像素)
    Json::Value dumpSemanticTree(int maxDepth = -1) const;

    void setObjectName(const std::string&);  // 已有? 若无则补 — selector 的锚点
};

struct JZAiAction {
    std::string name;      // "click" / "setValue" / "select" / "scroll"
    Json::Value paramsSchema;  // JSON Schema (直接喂 MCP tool schema)
};
```

各控件的重写要点 (量小, 每个控件 5~10 行):
- JZPushButton: role=button, actions=[click];
- JZSlider: role=slider, value=当前值, actions=[setValue];
- JZCheckbox/JZSwitch/JZRadioButton: role=checkbox/switch/radio, value=checked, actions=[toggle];
- JZComboBox: role=combo, value=当前项, actions=[select(index|text)];
- JZTextEdit/JZLabel: role=textbox/label, value=text, actions=[setText];
- JZListWidget: role=list, value=children 摘要, actions=[selectItem, scroll];
- JZTableWidget: role=table, value=可见区摘要, actions=[selectCell, scroll];

语义树 JSON 形态:
```json
{ "role": "window", "name": "JZWidget Widgets", "rect": [0,0,800,600],
  "children": [
    { "role": "list", "name": "widget-list", "value": {"current": 0, "items": ["Label", ...]},
      "children": [...] },
    { "role": "button", "name": "Update", "actions": [{"name":"click"}] } ] }
```

### L2 驱动层 — AI 的手

```cpp
class JZUiDriver : public JZObject {
public:
    explicit JZUiDriver(JZWindow* root);

    // 读: 可在任意线程调用 (内部快照, 无锁读语义树的红利: UI 线程外只读不可变副本)
    Json::Value find(const std::string& selector);       // 语义节点摘要
    Json::Value read(const std::string& selector);       // 该节点 value 详情

    // 动: 必须转 UI 线程执行 —— act() 把动作打包成 JZEvent post 给 window,
    // UI 线程 event 分发时真正 invoke 控件, 完成后 sigActionDone 回 AI 线程。
    void act(const std::string& selector, const std::string& action,
             const Json::Value& args = Json::Value());

    // 观察: 语义树变化流 (update()/setVisible/setValue 时合并节流, 200ms 窗口)
    JZSigJson sigSemanticChanged;    // 增量: 变化节点子树

    // 选择器语法 (最小实现):
    //   "#name"  按对象名;  "role" 按角色;  "role#name";  "#name/child-role" 路径
    JZWidget* resolve(const std::string& selector);
};
```

关键线程模型 (与现有机制完全对齐):
- **写操作一律经事件队列**: 复用 `JZApplication::postEvent` + 自定义 `JZUiActEvent`,
  UI 线程在 `JZWin32Thread::run()` 的 `processEvents()` 环节消费 —— 零新增线程基础设施;
- **信号跨线程回送**: `sigActionDone/sigSemanticChanged` 用 AutoConnection, 接收方在
  AI 线程时自动 Queued —— JZObject 已支持;
- **读操作**: `dumpSemanticTree` 在 UI 线程生成快照 (act/read 也统一走 UI 线程最简单,
  第一版全部投递到 UI 线程, 避免锁)。

### L3 协议层 — 标准出口

1. **MCP tools** (JZUiMcpBridge, 组合 JZMcpServer + JZUiDriver):
   - `ui_dump_tree` — 全量/子树语义树;
   - `ui_read(selector)` — 节点详情;
   - `ui_act(selector, action, args)` — 调动作 (带权限检查);
   - `ui_screenshot` — (后期) GDI+ 位图 → PNG, 给多模态模型;
   - 事件流: WS 传输的 `notifications/ui_changed` (sigSemanticChanged → 推送)。
2. **MCP 补全** (与主流客户端互通的前提, 独立小任务):
   - initialize/initialized 握手 + capabilities;
   - `resources/`: 语义树作为 `ui://tree` 资源;
   - stdio 传输 (本地 CLI agent 用)。
3. **WebSocket 事件流**: JZNet 已有 WS server, 直接桥 sigSemanticChanged。

### L4 生成层 — AI 建 UI

声明式 schema (与语义树同源, agent 可读也可写):

```json
{ "type": "vbox", "margin": 8, "spacing": 4, "children": [
    { "type": "label",   "id": "title", "text": "设备控制" },
    { "type": "slider",  "id": "volume", "min": 0, "max": 100, "value": 30 },
    { "type": "checkbox","id": "mute",   "text": "静音", "checked": false },
    { "type": "hbox", "children": [
        { "type": "push", "id": "ok", "text": "确定", "onClick": "apply" },
        { "type": "stretch" } ] } ] }
```

```cpp
class JZUiBuilder {
public:
    // schema -> 控件树; id 控件自动 setObjectName (selector/绑定的锚点)
    static JZWidget* fromJson(const Json::Value& schema, JZWidget* parent);
    // 事件接线: 把 schema 里的 "onClick": "apply" 绑到 handler
    //   handler 可以是: JZAgentTool / JS 回调 (JZJSEngine) / C++ std::function
    static void bindActions(JZWidget* root, JZObject* handler);
    // 热更新: diff 两份 schema, 最小改动控件树 (后期; 先做整树重建)
};
```

要点:
- 控件工厂注册表 `map<string, factoryFn>`, 新控件一行注册;
- schema 即 agent 的输出协议 —— JZAgent 配一个 `build_ui` tool, 参数就是 schema;
- 校验: 用 JZAgentTool 的 ParamSchema 机制约束 LLM 输出, 非法字段拒绝;
- 布局直接复用现有 JZBoxLayout/JZGridLayout (margin/spacing/stretch 语义已就绪)。

### L5 会话层 — UI 内嵌 AI

```cpp
// 流式聊天面板: JZAgent 的 sigReply(增量) -> append, 线程安全 (内部 postEvent)
class JZChatPanel : public JZWidget {
public:
    void attachAgent(JZAgent* agent);
    void sendUserText(const std::string& text);   // -> agent->ask()
    // 渲染: 气泡列表 (user/assistant), token 流 append, 打字指示
};

class JZMarkdownView : public JZWidget {   // 先支持: 标题/列表/代码块/粗体
};
```

JZAgent 需要的小改造: 流式增量信号 (processResponseDelta 已有, 确认 sigReply 是增量粒度)
+ `runTool` 时把 UI 动作工具挂进去。

---

## 3. 安全与权限

- **默认只读**: dump/read 永远开放; act 类动作需 `JZUiDriver::setActionsEnabled(true)` 显式开;
- **危险动作确认**: schema 里标 `"confirm": true` 的动作 → 自动弹 JZMessageBox (UI 线程天然能弹);
- **审计**: 每次 act 落一条日志 (时间/selector/action/args/结果), 复用 JZDebug;
- **作用域**: JZUiDriver 绑定单个 JZWindow, 跨窗口要显式注册 —— 对话框默认不被外部 agent 驱动。

---

## 4. 路线图 (每步都有可演示的产物)

| 里程碑 | 内容 | 演示 |
|---|---|---|
| **M1 看得见** | L1 语义接口 + dumpSemanticTree + selector resolve; 各控件重写 ai* | 命令行工具连上 WS, dump 出主窗口语义树 |
| **M2 动得了** | L2 JZUiDriver (act 走 postEvent) + JZUiMcpBridge 4 个 tools; JZMcpServer 补 initialize/resources | 外部 MCP 客户端 (如 Claude Desktop) "点击 Slider 页并读值" |
| **M3 说得上** | L5 JZChatPanel + JZMarkdownView + JZAgent 增量流接线 | 框架内置 AI 助手面板, 流式回答 |
| **M4 建得出** | L4 JZUiBuilder schema + bindActions + build_ui agent tool | 对话框里让 AI "生成一个设备配置表单", 即时出现可用界面 |
| **M5 生态** | stdio MCP / 截图 tool / JS 脚本 agent (QuickJS) / JZSimulator 推送通道 / 审计 | QuickJS 写的 agent 操控模拟器界面 |

里程碑 1/2 与当前 GDI+ 后端无耦合 (纯逻辑层), 不阻塞窗口/渲染侧的后续演进。

---

## 5. 设计原则 (后续所有取舍的依据)

1. **事件队列是唯一的桥**: AI 永远不直接碰 UI 线程对象 —— 复用 postEvent, 不引入锁;
2. **语义树优先于像素**: 截图/坐标是补充 (M5), role/name/value 才是 AI 的主通道;
3. **MCP 是标准接口, 不发明私有协议**: 让通用 agent 客户端零成本接入;
4. **声明式 JSON 而非代码生成**: AI 建界面的唯一合法路径, 可校验、可热更、可 diff;
5. **反射最小化**: 不做全量反射, 只做"注册式导出" (一行把成员函数挂成 tool), 避免过度工程;
6. **每个里程碑独立可演示**: 不做一次性大重构。

---

## 6. 立即可做的小事 (M1 前置, 半天级)

1. JZWidget 补 `objectName` (若查无) + `dumpSemanticTree` 骨架 (先只导 role=name=类名);
2. 语义树 JSON 化后接进 JZMcpServer 做成第一个 tool `ui_dump_tree` (JZUiDriver 可以先不做,
   tool 内部直接 postEvent 生成快照) —— 这是整条链路的最小纵向切片;
3. 用 JZAgent 现成 tool-calling 循环挂这个 tool, 验证 "agent 能描述出当前界面"。
