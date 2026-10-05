// ---------------------------------------------------------------------------
// test_bteserver —— C++ 版 bteserver(见 BTEServer.h 的说明)。
//
//   test_bteserver            开界面跑事件循环(默认, 与原版一样最大化)
//   test_bteserver --dump     开界面 + 泵几轮事件后打印控件几何(排查布局用)
//   test_bteserver --clipcheck 同 --dump, 但故意把卡片容器摆到父控件外(验证裁剪), 留着窗口截图
//   test_bteserver --port N   换监听端口(默认 8001, 与原版一致)
// ---------------------------------------------------------------------------
#include <chrono>
#include <thread>

#include "JZApplication.h"
#include "BTEServer.h"
#include "JZDebug.h"

#include "JZMainWindow.h"
#include "JZLayout.h"
#include "JZScrollArea.h"
#include "JZScrollBar.h"
#include "JZListWidget.h"
#include "JZStd.h"

#ifdef HAS_JZMcpServer
#include "JZUiMcpTools.h"   // AI-Native: MCP 桥(ui_screenshot/ui_invoke 等), 见 app_main 末尾
#include "JZMcpCore.h"
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

int app_main(int argc, char* argv[])
{
    JZApplication app;

    bool dumpMode = false;
    bool clipCheckMode = false;
    int port = 8001;                  // 原版端口; --port N 可换(0 会被引擎拒绝, 见 BTEServer.h)
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--dump")
            dumpMode = true;
        else if (a == "--clipcheck")
            clipCheckMode = true;
        else if (a == "--port" && i + 1 < argc)
            port = atoi(argv[++i]);
    }

    BTEServer ui;                  // 由本函数持有: 网络回调会长期引用它(见 BTEServer.h 的说明)
    buildBTEServer(ui, port);      // 工程目录默认 exe 同目录下的 Data(见 BTEServer.h 的说明)
    if (!ui.main)
    {
        jzDebug("[FAIL] 建界面失败: %s\n", ui.error.c_str());
        return 1;
    }

    // 与原版一致: 最大化跑(8 列卡片要有宽度)。⚠ 引擎的最小/最大化/还原都要求原生窗口
    // 已经建出来(它们是直接 ShowWindow), 所以先 show() 再 showMaximized()。
    ui.main->show();
    ui.main->showMaximized();

#ifdef HAS_JZMcpServer
    // AI-Native: 起 MCP server(读 get_ui_state/ui_screenshot + 写 ui_invoke), 供外部自动化驱动。
    // ⚠ 端口与 test_widget(9602)错开: 两个示例可能同时开着。
    JZUiMcpBridge mcp(ui.main);
    mcp.core()->setInstructions(
        "BTEServer 界面骨架的自动化工具。先 ui_windows/get_ui_state 看控件树(className/viewId/"
        "boundsInScreen/能力位), 再 ui_invoke 驱动交互, ui_screenshot 截图核对。");
    if (!mcp.listenHttp("127.0.0.1", 9603))
        return 1;
#endif

    const int rc = app.exec();
    delete ui.gen.root;
    return rc;
}

int main(int argc, char* argv[])
{
    return app_main(argc, argv);
}
