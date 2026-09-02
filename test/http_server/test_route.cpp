// JZHttpServer 路由与请求 API 单元测试（不依赖网络/服务器）
// 覆盖: 精确匹配 / 动态参数 / 中途路径参数 / 通配(path) / 优先级 / 无匹配(404触发) / JZHttpRequest 访问器
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "JZHttpRoute.h"
#include "JZHttpHandler.h"
#include "JZHttpRequest.h"
#include "JZApplication.h"

using namespace std;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "[FAIL] %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            fflush(stderr); \
            exit(1); \
        } \
    } while (0)

// ---------------- 路由匹配单元测试 ----------------
class DummyHandler : public JZHttpHandler
{
public:
    void handle(JZHttpServerClient *, const JZHttpRequest &) override {}
};

static void test_route_match()
{
    JZHttpRoute router;
    DummyHandler hRoot, hUser, hPosts, hCatch;
    router.addRoute("/", &hRoot);
    router.addRoute("/user/<id>", &hUser);
    router.addRoute("/user/<id>/posts", &hPosts);
    router.addRoute("/<path:path>", &hCatch);

    JZHttpHandler *h = nullptr;
    std::map<std::string, std::string> p;

    // 1. 精确匹配
    p.clear(); h = nullptr;
    CHECK(router.match("/", h, p) && h == &hRoot, "exact / should match root");
    CHECK(p.empty(), "root has no params");

    // 2. 动态参数（末尾）
    p.clear(); h = nullptr;
    CHECK(router.match("/user/42", h, p) && h == &hUser, "dynamic /user/<id>");
    CHECK(p["id"] == "42", "param id should be 42");

    // 3. 中途路径参数（此前有 bug：会把 "42/posts" 整个当成 id）
    p.clear(); h = nullptr;
    CHECK(router.match("/user/42/posts", h, p) && h == &hPosts,
          "mid-path param /user/<id>/posts should match hPosts");
    CHECK(p["id"] == "42", "mid-path param id should be 42 (not 42/posts)");

    // 4. 通配 path
    p.clear(); h = nullptr;
    CHECK(router.match("/assets/a.png", h, p) && h == &hCatch, "catch-all path");
    CHECK(p["path"] == "assets/a.png", "catch-all path param");

    // 5. 优先级：更具体的路由应优先于通配
    p.clear(); h = nullptr;
    CHECK(router.match("/user/42/posts", h, p) && h == &hPosts,
          "specific route wins over catch-all");

    // 6. 无匹配 -> 返回 false（服务器据此返回 404）
    p.clear(); h = nullptr;
    CHECK(!router.match("/user", h, p), "/user without id should not match");
    CHECK(h == nullptr, "no handler for /user");
}

// ---------------- JZHttpRequest 访问器单元测试 ----------------
static void test_request_api()
{
    JZHttpRequest req;
    req.setMethodString("POST");
    CHECK(req.methodString() == "POST", "methodString POST");
    CHECK(req.method() == JZHttpRequest::POST, "method enum POST");

    req.setPath("/search");
    CHECK(req.path() == "/search", "path");

    req.setQuery("q=hello&page=2");
    CHECK(req.queryParam("q") == "hello", "queryParam q");
    CHECK(req.queryParam("page") == "2", "queryParam page");

    // 同名参数后者覆盖前者
    req.setQuery("x=1&x=2");
    CHECK(req.queryParam("x") == "2", "last duplicate query wins");

    req.setHeader("Content-Type", "text/plain");
    CHECK(req.header("Content-Type") == "text/plain", "header set/get");
    CHECK(req.contentType() == "text/plain", "contentType()");

    req.setParam("id", "42");
    CHECK(req.param("id") == "42", "route param");

    JZByteArray body;
    body.append("hello world", 11);
    req.setBody(body);
    std::string bodyStr(req.body().data(), req.body().size());
    CHECK(bodyStr == "hello world", "body");
}

int main()
{
    JZApplication app;
    test_route_match();
    test_request_api();
    printf("[OK] JZHttpServer route & request API unit tests passed\n");
    return 0;
}
