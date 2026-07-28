#pragma once
#include "JZHttpServer.h"
#include "JZHttpStaticFiles.h"
#include "JZJSWebApp.h"
#include <string>

// JZWebServer —— 本地 HTTP 服务。
//   - 静态资源挂载在 <exeDir>/app/webroot（根路径 /）
//   - 动态 API 路由 /api/<path> 直接派发给内嵌的 JZJSEngine（Node.js 风格环境）
//   - 启动时自动加载 <exeDir>/app/scripts 下的脚本
class JZWebServer
{
public:
    JZWebServer();
    ~JZWebServer();

    static std::string baseUrl();   // 如 "http://127.0.0.1:8080"
    static int port();

    JZJSWebApp* jsApp() { return &m_jsApp; }

private:
    static const int kPort = 8080;

    JZHttpServer m_server;
    JZHttpStaticFiles m_static;
    JZJSWebHandler m_apiHandler;  // 处理 /api/<path:rest>
    JZJSWebApp m_jsApp;           // 内嵌 JS 引擎 + 路由表（须在 m_server 前声明，先销毁）
};
