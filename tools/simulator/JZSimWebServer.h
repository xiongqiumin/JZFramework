#ifndef JZ_SIM_WEB_SERVER_H_
#define JZ_SIM_WEB_SERVER_H_

#include "JZObject.h"

class JZSimulator;
class JZHttpServer;

// 基于 JZHttpServer 的网页界面服务:
// - GET  /                      由 JZHttpStaticFiles 静态返回单独的文件 web/index.html (单页应用 HTML/CSS/JS)
// - GET  /api/state             返回完整状态 JSON (使用 JZJson 序列化)
// - GET  /api/log/<since>       返回从 since 起的日志 JSON
// - POST /api/name              body 为新设备名
// - POST /api/advertising/<on>  0/1 切换广播
// - POST /api/notify/<index>    body 为特征值, 向 App Central 通知
// - POST /api/connect           连接 App Peripheral
// - POST /api/disconnect        断开 App Peripheral
// - POST /api/writeperiph/<index> body 为特征值, 写入 App Peripheral
class JZSimWebServer : public JZObject
{
public:
    explicit JZSimWebServer(JZSimulator *sim, JZObject *parent = nullptr);
    ~JZSimWebServer() override;

    bool start(int port = 8080);

private:
    void setupRoutes();

    JZSimulator  *m_sim = nullptr;
    JZHttpServer *m_http = nullptr;
};

#endif // !JZ_SIM_WEB_SERVER_H_
