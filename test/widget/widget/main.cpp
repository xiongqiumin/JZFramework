#include "JZApplication.h"
#include "JZWindow.h"
#include "main_window.h"
#ifdef HAS_JZMcpServer
#include "JZUiMcpTools.h"
#include "JZMcpCore.h"
#endif

int app_main(int argc, char* argv[])
{
    JZApplication app;

    MainWindow window;            // JZWindow subclass: the window itself
    window.resize(800, 600);
    window.setTitle("JZWidget Widgets");
    window.show();   // not shown = not displayed

#ifdef HAS_JZMcpServer
    // AI-Native P2: MCP server — 读(get_ui_state/ui_screenshot) + 写(ui_invoke)
    JZUiMcpBridge mcp(&window);
    mcp.core()->setInstructions(
        "JZWidget 控件演示窗口的自动化工具。先 get_ui_state 看语义树 "
        "(className/viewId/text/boundsInScreen/能力位), 再 ui_invoke 驱动交互, "
        "ui_screenshot 截图核对结果。");
    if (!mcp.listenHttp(9601, "/mcp")) {
        return 1;
    }
    mcp.listenWebSocket(9602);
#endif

    return app.exec();
}

int main(int argc, char* argv[])
{
    return app_main(argc, argv);
}
