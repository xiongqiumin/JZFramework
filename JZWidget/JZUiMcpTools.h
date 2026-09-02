#ifndef JZ_UI_MCP_TOOLS_H_
#define JZ_UI_MCP_TOOLS_H_

// AI-Native L3 (ai-native-design.md): JZUiMcpBridge —— 组合 JZMcpServer + UI 语义工具。
// 仅当 JZMcpServer 模块参与构建时 (HAS_JZMcpServer, 见 JZWidget.cmake) 才编译本文件。
// 三个工具均由主线程事件循环驱动, run() 在 UI 线程执行, 直接操作控件树安全。

#include "JZMcpServer.h"
#include "JZMcpTool.h"

class JZWindow;
class JZAiDriver;

// get_ui_state: 导出无障碍语义树 (AccessibilityNodeInfo 风格 JSON)
class JZUiGetStateTool : public JZMcpTool
{
    JZ_OBJECT_DECLARE(JZUiGetStateTool)

public:
    explicit JZUiGetStateTool(JZWindow* root, JZObject* parent = nullptr);
    void run(const Json::Value& id, const Json::Value& args) override;

private:
    JZWindow* m_root;
};

// ui_screenshot: PrintWindow 抓根窗口当前帧 -> PNG, 返回文件路径
class JZUiScreenshotTool : public JZMcpTool
{
    JZ_OBJECT_DECLARE(JZUiScreenshotTool)

public:
    explicit JZUiScreenshotTool(JZWindow* root, JZObject* parent = nullptr);
    void run(const Json::Value& id, const Json::Value& args) override;

private:
    void screenshot(const Json::Value& id);   // UI 线程执行

    JZWindow* m_root;
};

// ui_invoke: 解析目标 (viewId/className) 执行动作。异步:
//   先由 JZAiDriver 把 AI 鼠标平滑移动到目标 (状态条提示, 可取消),
//   到达后执行 —— click/toggle 合成真实点击 (带涟漪), 其余直调。
//   完成或被取消后才通过 sigFinished 回送结果。
class JZUiInvokeTool : public JZMcpTool
{
    JZ_OBJECT_DECLARE(JZUiInvokeTool)

public:
    explicit JZUiInvokeTool(JZWindow* root, JZAiDriver* driver, JZObject* parent = nullptr);
    void run(const Json::Value& id, const Json::Value& args) override;
    void cancel(const Json::Value& id) override;

private:
    void invokeOnUi(const Json::Value& id, const Json::Value& args);   // UI 线程执行
    void onAiDone(int seq, bool ok, const std::string& msg);

private:
    JZWindow* m_root;
    JZAiDriver* m_driver;
    Json::Value m_pendingId;
    int m_pendingSeq = -1;
};

// JZUiMcpBridge: JZMcpServer + 上述三工具 (root 为语义树/截图/驱动的根)
class JZUiMcpBridge : public JZMcpServer
{
    JZ_OBJECT_DECLARE(JZUiMcpBridge)

public:
    explicit JZUiMcpBridge(JZWindow* root, JZObject* parent = nullptr);
    JZAiDriver* aiDriver() const { return m_driver; }

private:
    JZWindow* m_root;
    JZAiDriver* m_driver;
};

#endif // JZ_UI_MCP_TOOLS_H_
