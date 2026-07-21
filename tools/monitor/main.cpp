#include "JZApplication.h"
#include "JZDebug.h"
#include "JZMonitor.h"

// JZ BLE Simulator: 使用 JZFramework 事件循环驱动网络与 Web 服务
// 界面改由 JZHttpServer 提供的网页承载 (默认 http://127.0.0.1:8080)
int main(int argc, char *argv[])
{
    int webPort = 8080;   // 网页端口
    int monitorPort = 19123;  // 与驱动通信端口

    JZApplication app;
	
	JZMonitor monitor;
    monitor.start(webPort, monitorPort);

    if (!monitor.start(webPort, monitorPort)) {
        jzWarning() << "Web server failed to listen on port" << webPort;
        return 1;
    }
	
    return app.exec();
}
