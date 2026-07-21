#include "JZSimWebServer.h"
#include "JZSimulator.h"

#include "JZHttpServer.h"
#include "JZHttpRequest.h"
#include "JZHttpRespone.h"
#include "JZHttpHandler.h"
#include "JZHttpStaticFiles.h"
#include "JZByteArray.h"
#include "JZJson.h"

#include <functional>
#include <string>

// 网页根目录: 由 CMake 通过 JZ_SIM_WEB_ROOT 传入 (绝对路径);
// 这里提供默认回退, 使源码也可独立编译/被 IDE 解析。
#ifndef JZ_SIM_WEB_ROOT
#define JZ_SIM_WEB_ROOT "web"
#endif

// ======================== 通用 lambda 路由处理器 ========================
namespace {

class FuncHandler : public JZHttpHandler
{
public:
    using Fn = std::function<void(JZHttpServerClient *, const JZHttpRequest &)>;
    explicit FuncHandler(Fn fn) : m_fn(std::move(fn)) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override { m_fn(client, req); }
private:
    Fn m_fn;
};

// 取出请求 body (UTF-8 文本)
std::string bodyText(const JZHttpRequest &req)
{
    const JZByteArray &b = req.body();
    if (b.size() <= 0) return std::string();
    return std::string(b.data(), (size_t)b.size());
}

// 回复 JSON
void sendJson(JZHttpServerClient *client, const std::string &json)
{
    JZHttpRespone resp;
    resp.setStatusCode(200);
    resp.setContentType("application/json; charset=utf-8");
    resp.setBody(json);
    client->response(resp.toByteArray());
}

// 回复一个 ok:true 的 JSON
void sendOk(JZHttpServerClient *client)
{
    sendJson(client, R"({"ok":true})");
}

} // namespace

// ======================== JZSimWebServer ========================
JZSimWebServer::JZSimWebServer(JZSimulator *sim, JZObject *parent)
    : JZObject(parent), m_sim(sim)
{
}

JZSimWebServer::~JZSimWebServer()
{
    if (m_http) { delete m_http; m_http = nullptr; }
}

bool JZSimWebServer::start(int port)
{
    m_http = new JZHttpServer(this);
    if (!m_http->listen(port))
        return false;
    setupRoutes();
    return true;
}

void JZSimWebServer::setupRoutes()
{
    JZSimulator *sim = m_sim;

    // 首页: 从单独的文件 (web/index.html) 静态服务
    m_http->addRoute("/", new JZHttpStaticFiles(JZ_SIM_WEB_ROOT));

    // 状态
    m_http->addRoute("/api/state", new FuncHandler([sim](JZHttpServerClient *client, const JZHttpRequest &) {
        sendJson(client, sim->stateJson());
    }));

    // 日志 (增量)
    m_http->addRoute("/api/log/<since>", new FuncHandler([sim](JZHttpServerClient *client, const JZHttpRequest &req) {
        size_t since = 0;
        try { since = (size_t)std::stoul(req.param("since")); } catch (...) { since = 0; }
        sendJson(client, sim->logsJson(since));
    }));

    // 设备名
    m_http->addRoute("/api/name", new FuncHandler([sim](JZHttpServerClient *client, const JZHttpRequest &req) {
        sim->setDeviceName(bodyText(req));
        sendOk(client);
    }));

    // 广播开关
    m_http->addRoute("/api/advertising/<on>", new FuncHandler([sim](JZHttpServerClient *client, const JZHttpRequest &req) {
        sim->setAdvertising(req.param("on") == "1");
        sendOk(client);
    }));

    // 通知 App Central
    m_http->addRoute("/api/notify/<index>", new FuncHandler([sim](JZHttpServerClient *client, const JZHttpRequest &req) {
        size_t idx = 0;
        try { idx = (size_t)std::stoul(req.param("index")); } catch (...) { idx = 0; }
        sim->notifyChar(idx, bodyText(req));
        sendOk(client);
    }));

    // 连接 / 断开 App Peripheral
    m_http->addRoute("/api/connect", new FuncHandler([sim](JZHttpServerClient *client, const JZHttpRequest &) {
        sim->connectCentral();
        sendOk(client);
    }));
    m_http->addRoute("/api/disconnect", new FuncHandler([sim](JZHttpServerClient *client, const JZHttpRequest &) {
        sim->disconnectCentral();
        sendOk(client);
    }));

    // 写 App Peripheral 特征
    m_http->addRoute("/api/writeperiph/<index>", new FuncHandler([sim](JZHttpServerClient *client, const JZHttpRequest &req) {
        size_t idx = 0;
        try { idx = (size_t)std::stoul(req.param("index")); } catch (...) { idx = 0; }
        sim->writePeriphChar(idx, bodyText(req));
        sendOk(client);
    }));
}
