#ifndef JZ_UI_MCP_TOOLS_H_
#define JZ_UI_MCP_TOOLS_H_

// AI-Native L3 (ai-native-design.md): JZUiMcpBridge —— 组合 JZMcpServer + UI 语义工具。
// 仅当 JZMcpServer 模块参与构建时 (HAS_JZMcpServer, 见 JZWidget.cmake) 才编译本文件。
// 三个工具均由主线程事件循环驱动, run() 在 UI 线程执行, 直接操作控件树安全。

#include "JZMcpServer.h"
#include "JZMcpTool.h"

class JZWindow;

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
    JZWindow* m_root;
};

// ui_invoke: 解析目标 (viewId/className) 执行动作
//   click/toggle  — 在控件中心合成一次真实鼠标点击 (走完整命中测试)
//   setValue      — slider 数值 / textbox 文本
//   select        — combo/list 项索引
class JZUiInvokeTool : public JZMcpTool
{
    JZ_OBJECT_DECLARE(JZUiInvokeTool)

public:
    explicit JZUiInvokeTool(JZWindow* root, JZObject* parent = nullptr);
    void run(const Json::Value& id, const Json::Value& args) override;

private:
    JZWindow* m_root;
};

// JZUiMcpBridge: JZMcpServer + 上述三工具 (root 为语义树/截图/驱动的根)
class JZUiMcpBridge : public JZMcpServer
{
    JZ_OBJECT_DECLARE(JZUiMcpBridge)

public:
    explicit JZUiMcpBridge(JZWindow* root, JZObject* parent = nullptr);

private:
    JZWindow* m_root;
};

#endif // JZ_UI_MCP_TOOLS_H_
