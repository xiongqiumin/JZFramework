#include "JZApplication.h"
#include "JZDebug.h"
#include "JZMonitor.h"

// JZ Monitor: 使用 JZFramework 事件循环驱动 HTTP 服务与 TCP 监控
//   Web 控制台 : http://127.0.0.1:<webPort>
//   监控通信    : tcp://127.0.0.1:<monitorPort>
int main(int argc, char *argv[])
{
    int webPort = 8080;       // 网页端口
    int monitorPort = 19123;  // 监控通信端口

    JZApplication app;

    JZMonitor monitor;
    if (!monitor.start(webPort, monitorPort)) {
        jzWarning() << "Monitor failed to start";
        return 1;
    }

    jzDebug() << "JZ Monitor running";
    jzDebug() << "  Web UI      : http://127.0.0.1:" << webPort;
    jzDebug() << "  Monitor port:" << monitorPort;

    return app.exec();
}
