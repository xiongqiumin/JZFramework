// desktop —— agent_cli 的桌面壳(WebView2 COM + 源码内嵌 server)
//
// 不再跑 standalone cli 的 HTTP/网页: 直接以源码引用方式把 JZAgentCliServer 跑在
// 本进程的一个线程里, 配置一个 inproc cli 作为唯一 worker, WebView2 打开 server 页面。
// 模型/工作区从 exe 同目录 config.json 读取(兼容旧 selfAgent 平铺字段与 llm.models)。
// 失败时退化打开 test_page.html。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include "JZApplication.h"
#include "JZThread.h"
#include "JZAgentCliServer.h"
#include "JZServerExtraTools.h"
#include "JZJson.h"
#include "JZWebView2Host.h"

#include <stdio.h>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>

// 让系统分配一个空闲端口(Windows 默认临时端口段 49152-65535)。
static int pickFreePort()
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 0;

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return 0;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    int port = 0;
    if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
        int len = sizeof(addr);
        if (getsockname(sock, reinterpret_cast<sockaddr *>(&addr), &len) == 0)
            port = ntohs(addr.sin_port);
    }
    closesocket(sock);
    WSACleanup();
    return port;
}

static bool waitForHttpPort(int port, int timeoutMs)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return false;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    bool ready = false;
    while (std::chrono::steady_clock::now() < deadline) {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock != INVALID_SOCKET) {
            sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(static_cast<USHORT>(port));
            if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
                closesocket(sock);
                ready = true;
                break;
            }
            closesocket(sock);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    WSACleanup();
    return ready;
}

class DesktopServerThread : public JZThread
{
public:
    explicit DesktopServerThread(const JZCliServerConfig &config)
        : m_config(config)
    {
    }

    void stopServer()
    {
        if (isRunning()) {
            quit();
            wait();
        }
    }

protected:
    void run() override
    {
        JZAgentCliServer server;
        jzRegisterServerExtraTools(&server);

        std::string err;
        if (!server.init(m_config, err)) {
            fprintf(stderr, "[desktop] server init failed: %s\n", err.c_str());
            jzUnregisterServerExtraTools();
            return;
        }
        if (!server.start(err)) {
            fprintf(stderr, "[desktop] server start failed: %s\n", err.c_str());
            jzUnregisterServerExtraTools();
            return;
        }

        JZThread::run();   // server 析构会 stop/join inproc worker
        jzUnregisterServerExtraTools();
    }

private:
    JZCliServerConfig m_config;
};

static std::string fileUrlFromPath(const std::string &path)
{
    std::string url = "file:///";
    for (char c : path) {
        if (c == '\\')
            url += '/';
        else if (c == ' ')
            url += "%20";
        else
            url += c;
    }
    return url;
}

// 从 config.json 取模型与工作区, 现场生成:
//   1) server 配置(明文 loopback, HTTP/hub 各一个随机高位端口)
//   2) 唯一 inproc cli 的配置文件(exeDir/desktop_cli.json)
static bool buildDesktopServerConfig(const std::string &exeDir,
                                     JZCliServerConfig &config,
                                     std::string &error)
{
    std::ifstream ifs(exeDir + "/config.json", std::ios::binary);
    if (!ifs) {
        error = "cannot open config.json";
        return false;
    }

    Json::Value root;
    Json::CharReaderBuilder builder;
    if (!Json::parseFromStream(builder, ifs, &root, &error))
        return false;

    std::string modelName;
    std::string modelUrl;
    std::string modelToken;
    std::string rootDir;
    int contextWindow = 160000;
    int netTimeout = 120000;
    int maxRounds = 100;

    const Json::Value &self = root["selfAgent"];
    if (self.isObject()) {
        modelName = self.get("modelName", "").asString();
        modelUrl = self.get("modelUrl", "").asString();
        modelToken = self.get("modelToken", "").asString();
        rootDir = self.get("rootDir", "").asString();
        contextWindow = self.get("maxToken", contextWindow).asInt();
        netTimeout = self.get("netTimeout", netTimeout).asInt();
        maxRounds = self.get("maxRounds", maxRounds).asInt();
    } else {
        const Json::Value &models = root["llm"]["models"];
        if (models.isArray() && !models.empty()) {
            const Json::Value &m = models[0];
            modelName = m.get("name", "").asString();
            modelUrl = m.get("url", "").asString();
            modelToken = m.get("token", "").asString();
            contextWindow = m.get("contextWindow", contextWindow).asInt();
        }
        rootDir = root["terminal"]["rootDir"].asString();
    }

    if (modelName.empty() || modelUrl.empty()) {
        error = "config.json has no model name/url";
        return false;
    }
    if (contextWindow <= 0)
        contextWindow = 160000;
    if (rootDir.empty())
        rootDir = exeDir + "/workspace";

    const std::string cliCfgPath = exeDir + "/desktop_cli.json";
    Json::Value cli(Json::objectValue);
    cli["agentName"] = "server";
    Json::Value model(Json::objectValue);
    model["name"] = modelName;
    model["url"] = modelUrl;
    model["token"] = modelToken;
    model["contextWindow"] = contextWindow;
    cli["llm"]["models"].append(model);
    cli["llm"]["netTimeoutMs"] = netTimeout;
    cli["llm"]["maxRounds"] = maxRounds;
    cli["terminal"]["rootDir"] = rootDir;

    std::ofstream ofs(cliCfgPath, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        error = "cannot write " + cliCfgPath;
        return false;
    }
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "  ";
    ofs << Json::writeString(wb, cli);
    ofs.close();

    int httpPort = pickFreePort();
    int hubPort = pickFreePort();
    while (hubPort > 0 && hubPort == httpPort)
        hubPort = pickFreePort();
    if (httpPort <= 0 || hubPort <= 0) {
        error = "cannot pick free ports";
        return false;
    }

    config.http.bind = "127.0.0.1";
    config.http.port = httpPort;
    config.http.sslEnabled = false;
    config.http.token.clear();

    config.hub.bind = "127.0.0.1";
    config.hub.port = hubPort;
    config.hub.token.clear();

    JZCliServerAgentConfig worker;
    worker.name = "server";
    worker.enabled = true;
    worker.dir = exeDir;
    worker.args = {"-c", cliCfgPath};
    worker.tools = {"server_agents", "server_task_query"};
    config.agents.push_back(worker);

    fprintf(stderr, "[desktop] server http=%d hub=%d cli_config=%s\n",
            httpPort, hubPort, cliCfgPath.c_str());
    return true;
}

int main(int argc, char *argv[])
{
    JZApplication app;
    const std::string exeDir = app.applicationDirPath();

    std::string url;
    bool serverStarted = false;
    DesktopServerThread *serverThread = nullptr;

    JZCliServerConfig serverConfig;
    std::string err;
    if (!buildDesktopServerConfig(exeDir, serverConfig, err)) {
        fprintf(stderr, "[desktop] build server config failed: %s\n", err.c_str());
    } else {
        serverThread = new DesktopServerThread(serverConfig);
        serverThread->start();
        serverStarted = true;
        if (!waitForHttpPort(serverConfig.http.port, 30000)) {
            fprintf(stderr, "[desktop] server did not listen on port %d within 30s\n",
                    serverConfig.http.port);
        } else {
            fprintf(stderr, "[desktop] server ready on port %d\n", serverConfig.http.port);
            url = "http://127.0.0.1:" + std::to_string(serverConfig.http.port) + "/";
            fprintf(stderr, "[desktop] opening server page: %s\n", url.c_str());
        }
    }

    if (url.empty()) {
        fprintf(stderr, "[desktop] fallback to local smoke page\n");
        url = fileUrlFromPath(exeDir + "/test_page.html");
    }

    JZWebView2Host wv;
    if (!wv.initialize(argc, argv)) {
        if (serverThread) {
            serverThread->stopServer();
            delete serverThread;
        }
        return 1;
    }
    if (!wv.createWindow(url)) {
        wv.shutdown();
        if (serverThread) {
            serverThread->stopServer();
            delete serverThread;
        }
        return 1;
    }

    const int ret = app.exec();
    wv.shutdown();

    if (serverStarted && serverThread) {
        fprintf(stderr, "[desktop] stopping in-process server\n");
        serverThread->stopServer();
        delete serverThread;
    }
    return ret;
}
