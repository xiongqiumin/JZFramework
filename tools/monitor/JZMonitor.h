#ifndef JZ_MONITOR_H_
#define JZ_MONITOR_H_

#include "JZObject.h"
#include "JZHttpServer.h"
#include "JZTcpServer.h"

// JZ Monitor 框架
//  - JZHttpServer : 提供 Web 控制台 (默认 http://127.0.0.1:8080)
//  - JZTcpServer  : 与驱动/被监控端通信
// 功能暂不实现, 仅搭建可编译骨架
class JZMonitor : public JZObject
{
    JZ_OBJECT_DECLARE(JZMonitor)

public:
    JZMonitor(JZObject *parent = nullptr);
    ~JZMonitor() override;

    // 启动: HttpServer 监听 webPort, TcpServer 监听 monitorPort
    bool start(int webPort, int monitorPort);
    void stop();

private:
    // TODO: 处理监控连接接入
    void onTcpAccept();

    JZHttpServer *m_http = nullptr;   // Web 控制台
    JZTcpServer  *m_tcp  = nullptr;   // 监控通信
};

#endif // !JZ_MONITOR_H_
