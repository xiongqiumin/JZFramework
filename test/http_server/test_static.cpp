#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "JZDebug.h"
#include "JZEventLoop.h"
#include "JZTimer.h"
#include "JZByteArray.h"
#include "JZHttpServer.h"
#include "JZHttpStaticFiles.h"
#include "JZNetworkAccessManager.h"
#include "JZNetworkReply.h"
#include "JZNetworkRequest.h"
#include "JZApplication.h"
#include "JZDir.h"

using namespace std;

#define LOG(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while(0)

static atomic<bool> g_active{true};

static void reportError(const string &where, const string &detail)
{
    if (!g_active.load())
        return;   // 测试已结束, 关闭 socket 等正常析构事件忽略
    jzDebug() << "[FATAL][" << where << "] " << detail;
    exit(1);
}

static bool byteEqual(const JZByteArray &a, const JZByteArray &b)
{
    if (a.size() != b.size())
        return false;
    return memcmp(a.data(), b.data(), (size_t)a.size()) == 0;
}

// 生成确定性的二进制内容（覆盖全字节，含 0x00），用于校验分块发送字节级一致
static JZByteArray makeContent(int size)
{
    JZByteArray b;
    b.resize(size);
    char *p = b.data();
    for (int i = 0; i < size; i++)
        p[i] = (char)((i * 31 + 7) & 0xFF);
    return b;
}

// 用 C 运行时直接落盘，绕过框架路径工具，保证测试夹具可靠创建
static void ensureDir(const string &path)
{
    JZDir dir;
    dir.mkpath(path);
}

static void writeFile(const string &path, const JZByteArray &data)
{
    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp)
    {
        reportError("PREPARE", "无法创建测试文件 " + path);
        return;
    }
    fwrite(data.data(), 1, (size_t)data.size(), fp);
    fclose(fp);
}

// ===================== 静态文件流式服务测试 =====================
class HttpStaticTest : public JZObject
{
public:
    HttpStaticTest(JZEventLoop *loop)
        : m_loop(loop)
    {
        int port = 18212;
        m_url = "http://127.0.0.1:" + to_string(port);

        prepareFiles();

        m_server = new JZHttpServer(this);
        m_server->listen(port);
        // catch-all 路由：所有路径交给静态文件 handler
        JZHttpStaticFiles *static_handler = new JZHttpStaticFiles(m_root);
        static_handler->setChunkSize(8192);   // 验证可配置每次发送大小
        m_server->addRoute("/<path:path>", static_handler);

        m_net = new JZNetworkAccessManager(this);

        m_exitTimer = new JZTimer(this);
        m_exitTimer->setSingleShot(true);
        connect(&m_exitTimer->sigTimeout, this, [this]() { m_loop->quit(); });
        m_exitTimer->start(30 * 1000);

        LOG("[test] server listening, fetching 404...");
        // 步骤 0：请求不存在的文件 -> 期望 404
        fetch("/not_exist.bin", 404, str_toUtf8("404 Not Found"));
    }

    void prepareFiles()
    {
        m_root = jzApp->applicationDirPath() + "/http_static_root";
        ensureDir(m_root);

        // 大文件（> 4k，触发多次分块发送）
        m_filePath = m_root + "/big.bin";
        m_expect = makeContent(50000);
        writeFile(m_filePath, m_expect);

        // 普通文本文件
        m_html = str_toUtf8("<html><body>hello</body></html>");
        writeFile(m_root + "/index.html", m_html);
    }

    void fetch(const string &path, int expectCode, const JZByteArray &expectBody)
    {
        m_expectCode = expectCode;
        m_expectBody = expectBody;

        JZNetworkRequest req(m_url + path);
        JZNetworkReply *reply = m_net->get(req);
        connect(&reply->sigFinished, this, [this, reply]() { onFinished(reply); });
    }

    void onFinished(JZNetworkReply *reply)
    {
        if (reply->error() != JZNetworkReply::NoError
            && reply->error() != JZNetworkReply::HttpError)
        {
            reportError("HTTP_STATIC", "net error: " + reply->errorString());
            return;
        }

        LOG("[test] onFinished step=%d", m_step);
        int code = reply->httpStatusCode();
        JZByteArray body = reply->readAll();
        if (code != m_expectCode)
            reportError("HTTP_STATIC", "状态码错误 期望=" + to_string(m_expectCode) + " 实际=" + to_string(code));
        if (!byteEqual(body, m_expectBody))
            reportError("HTTP_STATIC", "body 不匹配 期望长度=" + to_string(m_expectBody.size()) + " 实际=" + to_string(body.size()));
        reply->deleteLater();

        if (m_step == 0)
        {
            // 404 通过，接着测大文件（分块流式发送）
            LOG("[test] 404 OK, fetching big.bin (streaming)...");
            m_step = 1;
            fetch("/big.bin", 200, m_expect);
        }
        else if (m_step == 1)
        {
            // 大文件通过，测普通文本
            LOG("[test] big.bin streaming OK (%d bytes)", m_expect.size());
            m_step = 2;
            fetch("/index.html", 200, m_html);
        }
        else
        {
            LOG("[test] ALL OK");
            g_active = false;
            m_loop->quit();
            ::exit(0);   // 绕过可能的框架析构挂起，直接退出
        }
    }

    JZEventLoop *m_loop;
    string m_url, m_root, m_filePath;
    JZHttpServer *m_server;
    JZNetworkAccessManager *m_net;
    JZTimer *m_exitTimer;
    JZByteArray m_expect;   // 大文件期望内容
    JZByteArray m_html;     // index.html 内容
    int m_expectCode = 200;
    JZByteArray m_expectBody;
    int m_step = 0;
};

int main()
{
    JZApplication a;

    LOG("########## HTTP 静态文件流式发送测试 ##########");
    JZEventLoop loop;
    HttpStaticTest test(&loop);
    loop.exec();

    return 0;
}
