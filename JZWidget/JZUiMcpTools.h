#ifndef JZ_UI_MCP_TOOLS_H_
#define JZ_UI_MCP_TOOLS_H_

// AI-Native L3 (ai-native-design.md): JZUiMcpBridge —— 组合 JZMcpServer + UI 语义工具。
// 仅当 JZMcpServer 模块参与构建时 (HAS_JZMcpServer, 见 JZWidget.cmake) 才编译本文件。
// 全部工具均由主线程事件循环驱动, run() 在 UI 线程执行, 直接操作控件树安全。
//
// 工具集(都以"当前目标窗口"为操作对象, 可随时切换):
//   ui_windows          列出所有顶层窗口 (标题/viewId/可见/原生句柄/活跃/当前目标/位置尺寸)
//   ui_activate_window  切换并激活目标窗口 (target: '#viewId' / 标题子串 / 序号)
//   get_ui_state        导出当前目标窗口的无障碍语义树
//   ui_screenshot       抓当前目标窗口的帧 -> PNG
//   ui_invoke           在当前目标窗口里驱动控件交互 (AI 鼠标可见移动 + 合成点击/改值)
//
// 目标窗口由 JZUiMcpBridge 持有 (root()/setRoot()), 各工具每次 run 时取 —— 切换立刻生效,
// 不必重建服务。多窗口工程 (主窗口 + 对话框/浮层) 先 ui_windows 看清局面再切目标。

#include "JZMcpServer.h"
#include "JZMcpTool.h"

class JZWindow;
class JZAiDriver;
class JZUiMcpBridge;

// get_ui_state: 导出无障碍语义树 (AccessibilityNodeInfo 风格 JSON)
class JZUiGetStateTool : public JZMcpTool
{
    JZ_OBJECT_DECLARE(JZUiGetStateTool)

public:
    explicit JZUiGetStateTool(JZUiMcpBridge* bridge, JZObject* parent = nullptr);
    void run(const Json::Value& id, const Json::Value& args) override;

private:
    JZUiMcpBridge* m_bridge;
};

// ui_screenshot: PrintWindow 抓当前目标窗口当前帧 -> PNG, 返回文件路径
class JZUiScreenshotTool : public JZMcpTool
{
    JZ_OBJECT_DECLARE(JZUiScreenshotTool)

public:
    explicit JZUiScreenshotTool(JZUiMcpBridge* bridge, JZObject* parent = nullptr);
    void run(const Json::Value& id, const Json::Value& args) override;

private:
    void screenshot(const Json::Value& id, JZWindow* root);   // UI 线程执行

    JZUiMcpBridge* m_bridge;
};

// ui_windows: 列出顶层窗口 (含 active=系统活跃窗口 / current=本服务的当前目标窗口)
class JZUiWindowsTool : public JZMcpTool
{
    JZ_OBJECT_DECLARE(JZUiWindowsTool)

public:
    explicit JZUiWindowsTool(JZUiMcpBridge* bridge, JZObject* parent = nullptr);
    void run(const Json::Value& id, const Json::Value& args) override;

private:
    JZUiMcpBridge* m_bridge;
};

// ui_activate_window: 切换并激活目标窗口 (之后 get_ui_state/ui_screenshot/ui_invoke 都作用于它)
class JZUiActivateWindowTool : public JZMcpTool
{
    JZ_OBJECT_DECLARE(JZUiActivateWindowTool)

public:
    explicit JZUiActivateWindowTool(JZUiMcpBridge* bridge, JZObject* parent = nullptr);
    void run(const Json::Value& id, const Json::Value& args) override;

private:
    JZUiMcpBridge* m_bridge;
};

// ui_invoke: 解析目标 (viewId/className) 执行动作。异步:
//   先由 JZAiDriver 把 AI 鼠标平滑移动到目标 (状态条提示, 可取消),
//   到达后执行 —— click/toggle 合成真实点击 (带涟漪), 其余直调。
//   完成或被取消后才通过 sigFinished 回送结果。
class JZUiInvokeTool : public JZMcpTool
{
    JZ_OBJECT_DECLARE(JZUiInvokeTool)

public:
    explicit JZUiInvokeTool(JZUiMcpBridge* bridge, JZObject* parent = nullptr);
    void run(const Json::Value& id, const Json::Value& args) override;
    void cancel(const Json::Value& id) override;

private:
    void invokeOnUi(const Json::Value& id, const Json::Value& args);   // UI 线程执行
    void onAiDone(int seq, bool ok, const std::string& msg);

private:
    JZUiMcpBridge* m_bridge;
    Json::Value m_pendingId;
    int m_pendingSeq = -1;
};

// JZUiMcpBridge: JZMcpServer + 上述五个工具。
// root 只是"起始目标窗口"(一般取活跃窗口), 之后可用 ui_activate_window 切换。
class JZUiMcpBridge : public JZMcpServer
{
    JZ_OBJECT_DECLARE(JZUiMcpBridge)

public:
    explicit JZUiMcpBridge(JZWindow* root, JZObject* parent = nullptr);
    JZAiDriver* aiDriver() const { return m_driver; }

    // ---- 目标窗口 (工具集的作用对象) ----
    // 当前目标 (可能已被 JS 关窗销毁, 用 liveRoot() 判定)
    JZWindow* root() const { return m_root; }
    // 仍存活的目标窗口: 不在顶层窗口表里(已销毁)时返回 nullptr。
    // 只比指针, 绝不访问对象 —— 工具拿它之前必须先经这里。
    JZWindow* liveRoot() const;
    // 切换目标窗口: 激活它(前置+焦点) + 把 AI 覆盖层迁到新窗口(进行中的序列作废)。
    // 传 nullptr 只解绑(不激活任何窗口)。
    void setRoot(JZWindow* root);
    // 解析窗口: "#viewId" 按对象名 / 全数字按序号(0 基) / 其余按标题子串(不区分大小写)
    JZWindow* findWindow(const std::string& target) const;

private:
    JZWindow* m_root;
    JZAiDriver* m_driver;
};

#endif // JZ_UI_MCP_TOOLS_H_
