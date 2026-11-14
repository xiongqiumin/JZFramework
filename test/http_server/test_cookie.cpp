#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "JZApplication.h"
#include "JZEventLoop.h"
#include "JZTimer.h"
#include "JZByteArray.h"
#include "JZDebug.h"
#include "JZHttpServer.h"
#include "JZHttpHandler.h"
#include "JZHttpCookie.h"
#include "JZNetworkAccessManager.h"
#include "JZNetworkReply.h"
#include "JZNetworkRequest.h"
#include "JZStd.h"

using namespace std;

#define LOG(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static atomic<bool> g_active{true};

static void fail(const string &where, const string &detail)
{
    if (!g_active.load())
        return;
    jzDebug() << "[FATAL][" << where << "] " << detail;
    exit(1);
}

static bool expectEq(const string &where, const string &name,
                     const string &actual, const string &expect)
{
    if (actual == expect)
        return true;
    fail(where, name + " 期望='" + expect + "' 实际='" + actual + "'");
    return false;
}

// 统计子串出现次数(用于确认多条 Set-Cookie 都发出去了)
static int countOf(const string &text, const string &sub)
{
    int n = 0;
    size_t pos = 0;
    while ((pos = text.find(sub, pos)) != string::npos)
    {
        n++;
        pos += sub.size();
    }
    return n;
}

// ===================== 单元: 请求解析 / 响应序列化 =====================
static void testCookieUnit()
{
    // ---- 请求: 多 cookie + 空格 + 引号 + 拆成多行 Cookie 头 ----
    JZHttpRequest req;
    req.setHeader("Cookie", " sid=abc ; theme=\"dark\"");
    req.setHeader("cookie", "lang=zh-CN");
    expectEq("UNIT", "req.cookie(sid)", req.cookie("sid"), "abc");
    expectEq("UNIT", "req.cookie(theme)", req.cookie("theme"), "dark");
    expectEq("UNIT", "req.cookie(lang)", req.cookie("lang"), "zh-CN");
    if (req.cookies().size() != 3)
        fail("UNIT", "req.cookies().size() 期望=3 实际=" + to_string(req.cookies().size()));

    // 无 Cookie 头时为空
    JZHttpRequest empty;
    if (!empty.cookies().empty() || !empty.cookie("sid").empty())
        fail("UNIT", "无 Cookie 头时应为空");

    // ---- 响应: 多条 Set-Cookie 都能进报文 ----
    JZHttpRespone resp;
    JZHttpCookie sid("sid", "user-42");
    sid.path = "/";
    sid.httpOnly = true;
    sid.sameSite = "Lax";
    resp.setCookie(sid);
    resp.setCookie("theme", "dark");
    resp.setHeader("Set-Cookie", "manual=1; Max-Age=0");
    resp.setText("ok");

    if (resp.cookieLines().size() != 3)
        fail("UNIT", "cookieLines().size() 期望=3 实际=" + to_string(resp.cookieLines().size()));

    JZByteArray raw = resp.toByteArray();
    string head(raw.data(), (size_t)raw.size());
    if (countOf(head, "Set-Cookie: ") != 3)
        fail("UNIT", "报文里 Set-Cookie 行数不对: " + head);
    if (head.find("Set-Cookie: sid=user-42; Path=/; HttpOnly; SameSite=Lax\r\n") == string::npos)
        fail("UNIT", "sid 的属性序列化不对: " + head);
    if (head.find("Set-Cookie: theme=dark\r\n") == string::npos)
        fail("UNIT", "theme 序列化缺失: " + head);
    if (head.find("Set-Cookie: manual=1; Max-Age=0\r\n") == string::npos)
        fail("UNIT", "setHeader(Set-Cookie) 未按追加处理: " + head);

    // ---- removeHeader(Set-Cookie) 清空全部 ----
    resp.removeHeader("Set-Cookie");
    if (!resp.cookieLines().empty())
        fail("UNIT", "removeHeader(\"Set-Cookie\") 未清空 cookie");

    // ---- 客户端 jar: 域 / 路径 / Secure / 删除 ----
    int64_t now = (int64_t)std::time(nullptr) * 1000;

    {
        JZNetworkCookieJar jar;
        vector<string> hs;
        hs.push_back("sid=1; Path=/; HttpOnly");
        hs.push_back("scoped=2; Path=/api");
        hs.push_back("sec=3; Secure");
        jar.saveFromResponse(JZUrl("http://a.com/x"), hs, now);
        if (jar.count() != 3)
            fail("UNIT", "jar 条数期望=3 实际=" + to_string(jar.count()));

        expectEq("UNIT", "jar http /api/x", jar.headerFor(JZUrl("http://a.com/api/x"), now), "sid=1; scoped=2");
        expectEq("UNIT", "jar http /", jar.headerFor(JZUrl("http://a.com/"), now), "sid=1");
        expectEq("UNIT", "jar path 边界 /apix", jar.headerFor(JZUrl("http://a.com/apix"), now), "sid=1");
        expectEq("UNIT", "jar https 才带 Secure", jar.headerFor(JZUrl("https://a.com/"), now), "sid=1; sec=3");
        expectEq("UNIT", "jar 别的域不带", jar.headerFor(JZUrl("http://b.com/"), now), "");
    }

    {
        // 显式 Domain=.a.com: 才下发给 sub.a.com
        JZNetworkCookieJar jar;
        vector<string> hs;
        hs.push_back("wide=9; Domain=.a.com; Path=/");
        jar.saveFromResponse(JZUrl("http://a.com/x"), hs, now);
        expectEq("UNIT", "jar 子域匹配", jar.headerFor(JZUrl("http://sub.a.com/"), now), "wide=9");
    }

    {
        // 与 host 无关的 Domain 应被丢弃(防伪造别的域)
        JZNetworkCookieJar jar;
        vector<string> hs;
        hs.push_back("evil=1; Domain=other.com");
        jar.saveFromResponse(JZUrl("http://a.com/x"), hs, now);
        if (jar.count() != 0)
            fail("UNIT", "jar 应丢弃无关 Domain, 实际条数=" + to_string(jar.count()));
    }

    {
        // Max-Age=0: 删除同名同域同路径
        JZNetworkCookieJar jar;
        vector<string> hs;
        hs.push_back("sid=1; Path=/");
        jar.saveFromResponse(JZUrl("http://a.com/x"), hs, now);
        expectEq("UNIT", "jar 删除前", jar.headerFor(JZUrl("http://a.com/"), now), "sid=1");
        vector<string> hs2;
        hs2.push_back("sid=; Max-Age=0; Path=/");
        jar.saveFromResponse(JZUrl("http://a.com/x"), hs2, now);
        expectEq("UNIT", "jar Max-Age=0 删除", jar.headerFor(JZUrl("http://a.com/"), now), "");
    }

    {
        // Expires 已过 -> 不落库
        JZNetworkCookieJar jar;
        vector<string> hs;
        hs.push_back("gone=1; Expires=Wed, 21 Oct 2015 07:28:00 GMT");
        jar.saveFromResponse(JZUrl("http://a.com/x"), hs, now);
        if (jar.count() != 0)
            fail("UNIT", "jar 过期 cookie 不应落库, 实际条数=" + to_string(jar.count()));
    }

    LOG("[test] unit OK (解析/序列化/清除 + jar 域·路径·Secure·删除·过期)");
}

// ===================== 端到端: 下发 cookie 并按 cookie 认人 =====================
// 登录: 同时下发会话 cookie 与偏好 cookie(两条 Set-Cookie)
class LoginHandler : public JZHttpHandler
{
public:
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        JZHttpCookie sid("sid", "user-42");
        sid.path = "/";
        sid.httpOnly = true;
        sid.sameSite = "Lax";

        JZHttpRespone resp;
        resp.setCookie(sid);
        resp.setCookie("theme", "dark");
        resp.setJson("{\"ok\":true}");
        client->response(resp, req);
    }
};

// 认人: 有 sid 就返回对应用户, 没有就当匿名
class WhoAmIHandler : public JZHttpHandler
{
public:
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        string sid = req.cookie("sid");
        JZHttpRespone resp;
        resp.setText(sid.empty() ? string("anonymous") : ("user:" + sid));
        client->response(resp, req);
    }
};

// 登出: 用 Max-Age=0 让客户端删掉会话 cookie
class LogoutHandler : public JZHttpHandler
{
public:
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        JZHttpCookie sid("sid", "");
        sid.path = "/";
        sid.maxAge = 0;

        JZHttpRespone resp;
        resp.setCookie(sid);
        resp.setText("bye");
        client->response(resp, req);
    }
};

class CookieTest : public JZObject
{
public:
    CookieTest(JZEventLoop *loop)
        : m_loop(loop)
    {
        int port = 18231;
        m_url = "http://127.0.0.1:" + to_string(port);

        m_server = new JZHttpServer(this);
        if (!m_server->listen(port))
            fail("SETUP", "服务端 listen 失败");
        m_server->addRoute("/login", new LoginHandler());
        m_server->addRoute("/logout", new LogoutHandler());
        m_server->addRoute("/whoami", new WhoAmIHandler());

        m_net = new JZNetworkAccessManager(this);

        m_exitTimer = new JZTimer(this);
        m_exitTimer->setSingleShot(true);
        connect(&m_exitTimer->sigTimeout, this, [this]() {
            fail("TIMEOUT", "测试超时");
            m_loop->quit();
        });
        m_exitTimer->start(20 * 1000);

        LOG("[test] step0: GET /whoami (无 cookie)");
        m_step = 0;
        fetch("/whoami", "", "anonymous");
    }

    // cookieHeader 为空 = 不手动带, 交给 cookie jar 自动补
    void fetch(const string &path, const string &cookieHeader, const string &expectBody)
    {
        m_expectBody = expectBody;

        JZNetworkRequest req(m_url + path);
        if (!cookieHeader.empty())
            req.setRawHeader("Cookie", cookieHeader);

        JZNetworkReply *reply = m_net->get(req);
        connect(&reply->sigFinished, this, [this, reply]() { onFinished(reply); });
    }

    void fetchPath(const string &path, const string &expectBody)
    {
        m_expectBody = expectBody;
        JZNetworkRequest req(m_url + path);
        JZNetworkReply *reply = m_net->get(req);
        connect(&reply->sigFinished, this, [this, reply]() { onFinished(reply); });
    }

    void onFinished(JZNetworkReply *reply)
    {
        if (reply->error() != JZNetworkReply::NoError &&
            reply->error() != JZNetworkReply::HttpError)
            fail("HTTP", "net error: " + reply->errorString());

        int code = reply->httpStatusCode();
        string body = str_fromUtf8(reply->readAll());
        string head = reply->rawHeaders();
        if (code != 200)
            fail("HTTP", "状态码期望=200 实际=" + to_string(code));
        reply->deleteLater();

        if (m_step == 0)
        {
            expectEq("E2E", "无 cookie 时的身份", body, m_expectBody);
            LOG("[test] anonymous OK, step1: GET /login (下发两条 Set-Cookie)");
            m_step = 1;
            m_expectCookieCount = 2;
            fetchPath("/login", "{\"ok\":true}");
            return;
        }

        if (m_step == 1)
        {
            expectEq("E2E", "/login body", body, "{\"ok\":true}");
            if (countOf(head, "Set-Cookie: ") != m_expectCookieCount)
                fail("E2E", "Set-Cookie 条数期望=" + to_string(m_expectCookieCount) +
                            " 实际=" + to_string(countOf(head, "Set-Cookie: ")) + "\r\n" + head);
            if (head.find("Set-Cookie: sid=user-42; Path=/; HttpOnly; SameSite=Lax") == string::npos)
                fail("E2E", "会话 cookie 属性不对:\r\n" + head);
            if (head.find("Set-Cookie: theme=dark") == string::npos)
                fail("E2E", "偏好 cookie 缺失:\r\n" + head);
            if (m_net->cookieCount() != 2)
                fail("E2E", "客户端 jar 条数期望=2 实际=" + to_string(m_net->cookieCount()));

            LOG("[test] login OK (2 条 Set-Cookie 已存进 jar), step2: GET /whoami 靠 jar 自动带");
            m_step = 2;
            fetch("/whoami", "", "user:user-42");     // 不手动带 Cookie
            return;
        }

        if (m_step == 2)
        {
            expectEq("E2E", "自动带 cookie 时的身份", body, m_expectBody);
            LOG("[test] auto cookie OK, step3: GET /whoami 显式 Cookie 头应优先");
            m_step = 3;
            fetch("/whoami", "sid=intruder", "user:intruder");
            return;
        }

        if (m_step == 3)
        {
            expectEq("E2E", "显式 Cookie 头优先", body, m_expectBody);
            LOG("[test] explicit header OK, step4: GET /logout (Max-Age=0 删会话 cookie)");
            m_step = 4;
            fetchPath("/logout", "bye");
            return;
        }

        if (m_step == 4)
        {
            expectEq("E2E", "/logout body", body, "bye");
            if (m_net->cookieCount() != 1)
                fail("E2E", "登出后应只剩 theme 一条, 实际=" + to_string(m_net->cookieCount()));
            LOG("[test] logout OK (会话 cookie 已删), step5: GET /whoami");
            m_step = 5;
            fetch("/whoami", "", "anonymous");
            return;
        }

        expectEq("E2E", "登出后的身份", body, m_expectBody);
        LOG("[test] whoami OK (%s), ALL DONE", body.c_str());
        g_active = false;
        m_loop->quit();
        ::exit(0);
    }

    JZEventLoop *m_loop;
    string m_url;
    JZHttpServer *m_server;
    JZNetworkAccessManager *m_net;
    JZTimer *m_exitTimer;
    string m_expectBody;
    int m_expectCookieCount = 0;
    int m_step = 0;
};

int test_cookie()
{
    LOG("########## HTTP Cookie 测试 (解析 / 多条 Set-Cookie / 按 cookie 认人) ##########");
    testCookieUnit();

    g_active = true;
    JZEventLoop loop;
    CookieTest test(&loop);
    loop.exec();
    return 0;
}
