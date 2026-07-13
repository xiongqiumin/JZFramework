#include <string>
#include <vector>

#include "JZDebug.h"
#include "JZApplication.h"
#include "JZByteArray.h"

// ===================== 协程网络库 =====================
#include "JZCo.h"
#include "JZCoNetworkAccessManager.h"

// ===================== 普通 HTTP 服务器(供协程 HTTP 客户端测试) =====================
#include "JZHttpServer.h"
#include "JZHttpRequest.h"
#include "JZHttpRespone.h"
#include "JZHttpHandler.h"
#include "JZNetworkRequest.h"
#include "JZNetworkReply.h"

// ===================== 测试公用工具 =====================
#include "co_test_common.h"    // CO_TEST_ASSERT

using namespace std;

// 协程 HTTP 客户端测试用的普通(非协程)回声 Handler: 原样回传请求 body
class CoHttpEchoHandler : public JZHttpHandler
{
public:
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        JZHttpRespone resp;
        resp.setStatusCode(200);
        resp.setContentType("text/plain");
        resp.setBody(req.body());
        client->response(resp.toByteArray());
    }
};

// ============ 协程 HTTP: 用 JZCoNetworkAccessManager 做 POST echo ============
static void testCoHttp()
{
    jzDebug() << "########## 协程 HTTP 测试 (POST echo) ##########";
    const int port = 18204;
    const int rounds = 50;

    // 普通 HTTP 回声服务器(跑在事件循环上, 非协程)
    JZHttpServer server;
    CO_TEST_ASSERT(server.listen(port), "HTTP server listen 失败");
    server.addRoute("/test", new CoHttpEchoHandler());

    auto co = jzco_spawn([&]() {
        JZCoNetworkAccessManager nam;
        JZNetworkRequest req("http://127.0.0.1:" + to_string(port) + "/test");
        for (int i = 0; i < rounds; i++)
        {
            JZByteArray body = str_toUtf8(coMakeMsg("http", i));
            JZCoNetworkReply *reply = nam.post(req, body);
            reply->waitFinished();
            JZByteArray resp = reply->readAll();
            uint64_t seq;
            CO_TEST_ASSERT(coParseMsg(str_fromUtf8(resp), "http", seq) && seq == (uint64_t)i,
                           "协程 HTTP 回显校验失败 期望=" + to_string(i) + " 收到=" + str_fromUtf8(resp));
        }
    });

    jzco_waitCoroutine({co});
    jzDebug() << "【OK】协程 HTTP 回环通过 (" << rounds << " 轮 echo)";
}

// ===================== 测试入口 (供 test_co.cpp 编排调用) =====================
int test_coHttp()
{
    testCoHttp();
    return 0;
}
