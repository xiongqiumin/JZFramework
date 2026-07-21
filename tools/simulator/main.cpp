#include "JZApplication.h"
#include "JZDebug.h"
#include "JZSimulator.h"
#include "JZSimWebServer.h"

// JZ BLE Simulator: 使用 JZFramework 事件循环驱动网络与 Web 服务
// 界面改由 JZHttpServer 提供的网页承载 (默认 http://127.0.0.1:8080)
int main(int argc, char *argv[])
{
    int webPort = 8080;   // 网页端口
    int simPort = 19123;  // 与驱动通信端口

    JZApplication app;

    // 模型/控制器: BLE 仿真逻辑 + 与驱动的网络通信
    JZSimulator sim;
    sim.start(simPort);

    // 界面: 基于 JZHttpServer 的网页
    JZSimWebServer web(&sim);
    if (!web.start(webPort)) {
        jzWarning() << "Web server failed to listen on port" << webPort;
        return 1;
    }

    jzDebug() << "JZ BLE Simulator running";
    jzDebug() << "  Web UI     : http://127.0.0.1:" << webPort;
    jzDebug() << "  Driver port:" << simPort;

    return app.exec();
}
