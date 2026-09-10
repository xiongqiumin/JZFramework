// test_sse.cpp
//
// 向 net_test_server.py 的 3 个 SSE 端点各发 10 次请求, 校验客户端
// (JZNetworkAccessManager) 对三种 HTTP chunked 流结束方式的处理:
//   /sse-normal     情况1: 发 `0\r\n\r\n` 终止块后保持连接 (keep-alive, 不中断)
//   /sse-term-close 情况2: 发 `0\r\n\r\n` 终止块后关闭连接 (干净结束)
//   /sse-abrupt     情况3: 直接断连, 不发 `0\r\n\r\n` (chunked 被截断)
//
// 预期(对照 RFC7230 严格实现):
//   - 情况1/2: 收到零尺寸块, 响应正常完成 -> NoError, body 含两条 SSE 事件
//   - 情况3: chunked 未收到终止块即连接关闭, 客户端按"分块被截断"严格判 ConnectError
//            (此为符合规范的预期行为, 非 bug; 真实客户端普遍容忍, 但本框架选择严格拒绝)
//
// 依赖: 先启动 net_test_server.py (监听 8081 的 SSE 服务)
// 用法: test_sse [--host 127.0.0.1] [--port 8081]

#include "JZDebug.h"
#include "JZEventLoop.h"
#include "JZTimer.h"
#include "JZByteArray.h"
#include "JZNetworkAccessManager.h"
#include "JZNetworkReply.h"
#include "JZNetworkRequest.h"

#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <cstdlib>

using namespace std;

static const string SSE_HOST = "127.0.0.1";
static const int    SSE_PORT = 8081;
static const int    REPEAT   = 10;

struct EndpointSpec
{
    string path;
    string desc;
    bool   expectNoError;   // true: 期望全部 NoError;  false: 期望全部 ConnectError(截断)
};

static const vector<EndpointSpec> g_specs = {
    {"/sse-normal",     "情况1 发 0\\r\\n\\r\\n 不中断 (keep-alive)", true},
    {"/sse-term-close", "情况2 发 0\\r\\n\\r\\n 后关闭 (干净结束)",   true},
    {"/sse-abrupt",     "情况3 直接断连 (不发 0\\r\\n\\r\\n)",        false},
};

class SseTester : public JZObject
{
public:
    JZEventLoop *m_loop;
    JZNetworkAccessManager *m_net;
    string m_host;
    int m_port;

    map<string, int> m_ok;                 // NoError 且 SSE 事件内容正确
    map<string, int> m_err;                // 其余(错误 / 内容缺失)
    map<string, map<int, int>> m_errCodes; // 端点 -> (错误码 -> 次数)
    int m_done = 0;
    int m_total;

    SseTester(JZEventLoop *loop, const string &host, int port)
        : m_loop(loop), m_host(host), m_port(port)
    {
        m_net = new JZNetworkAccessManager(this);
        m_total = (int)g_specs.size() * REPEAT;
    }

    void start()
    {
        for (auto &s : g_specs)
            for (int i = 0; i < REPEAT; i++)
                sendOne(s.path);
    }

    void sendOne(const string &path)
    {
        string url = "http://" + m_host + ":" + to_string(m_port) + path;
        JZNetworkRequest req(url);
        JZNetworkReply *reply = m_net->get(req);
        connect(&reply->sigFinished, this, [this, path, reply]() {
            onReply(path, reply);
        });
    }

    void onReply(const string &path, JZNetworkReply *reply)
    {
        JZNetworkReply::NetworkError err = reply->error();
        m_errCodes[path][(int)err]++;

        if (err == JZNetworkReply::NoError)
        {
            string body = str_fromUtf8(reply->readAll());
            if (body.find("sse-event-1") != string::npos &&
                body.find("sse-event-2") != string::npos)
            {
                m_ok[path]++;
            }
            else
            {
                m_err[path]++;
                jzDebug() << "[WARN] " << path
                          << " NoError 但 SSE 事件内容缺失, body=" << body;
            }
        }
        else
        {
            m_err[path]++;
            jzDebug() << "[ERR] " << path << " error=" << (int)err
                      << " (" << reply->errorString() << ")";
        }

        reply->deleteLater();

        if (++m_done >= m_total)
            m_loop->quit();
    }
};

int main(int argc, char **argv)
{
    string host = SSE_HOST;
    int port = SSE_PORT;
    for (int i = 1; i < argc; i++)
    {
        string a = argv[i];
        if (a == "--host" && i + 1 < argc)
            host = argv[++i];
        else if (a == "--port" && i + 1 < argc)
            port = atoi(argv[++i]);
    }

    jzDebug() << "==== SSE 三端点测试: 每端点 " << REPEAT << " 次 (需先运行 net_test_server.py) ====";
    jzDebug() << "目标 http://" << host << ":" << port;

    JZEventLoop loop;
    SseTester tester(&loop, host, port);
    tester.start();

    // 安全超时: 30s 内未全部完成则强制退出(避免异常挂死)
    JZTimer timer(&loop);
    timer.setSingleShot(true);
    connect(&timer.sigTimeout, &tester, [&tester]() {
        jzDebug() << "[TIMEOUT] 30s 内未收齐全部响应, 强制退出";
        tester.m_loop->quit();
    });
    timer.start(30000);

    loop.exec();

    // 报告
    bool allPass = true;
    for (auto &s : g_specs)
    {
        int ok = tester.m_ok[s.path];
        int err = tester.m_err[s.path];
        bool pass;
        if (s.expectNoError)
            pass = (ok == REPEAT && err == 0);
        else
            // 情况3: 期望全部为 ConnectError(分块截断)
            pass = (tester.m_errCodes[s.path][(int)JZNetworkReply::ConnectError] == REPEAT);

        if (!pass)
            allPass = false;

        jzDebug() << (pass ? "[PASS]" : "[FAIL]") << " " << s.path
                  << " (" << s.desc << ")  ok=" << ok << " err=" << err;
        for (auto &kv : tester.m_errCodes[s.path])
            jzDebug() << "        errorCode=" << kv.first << " x" << kv.second;
    }

    jzDebug() << (allPass ? "【OK】SSE 三端点测试全部通过" : "【FAIL】存在不符合预期的端点");
    return allPass ? 0 : 1;
}
