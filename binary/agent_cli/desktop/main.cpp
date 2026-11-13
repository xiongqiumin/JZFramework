// desktop —— agent_cli 的桌面壳(当前是 CEF 占位实现, 见 JZCefHost.h)
//
// 方向: desktop 与 agent_cli 编译在同一份代码里, 页面走本地 HTTP(CEF 加载 file:// 会撞 CORS)。
// 这一版先摆好壳的形状: 不接真浏览器内核, 也不再依赖 JZWidget 自绘 UI,
// 启动后打印一行提示就退出。URL 可用第一个命令行参数覆盖。
//
// 真 CEF 接进来时只替换 JZCefHost 的实现, 本文件不用改。

#include "JZApplication.h"
#include "JZCefHost.h"

#include <string>

int main(int argc, char *argv[])
{
    JZApplication app;

    const std::string url = (argc > 1) ? argv[1] : "http://127.0.0.1:8088/";

    JZCefHost cef;
    if (!cef.initialize(argc, argv))
        return 1;
    cef.createWindow(url);
    const int ret = cef.runMessageLoop();
    cef.shutdown();
    return ret;
}
