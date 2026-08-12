#ifndef JZ_HTTP_ROUTE_H_
#define JZ_HTTP_ROUTE_H_

#include <string>
#include <vector>
#include <map>
#include <functional>
#include "JZHttpRequest.h"
#include "JZHttpRespone.h"
#include "JZHttpHandler.h"

/**
 * JZHttpRoute — 路由匹配器（类 Flask 语法）
 *
 * 三种路由模式：
 *
 *   1. Exact（精确匹配）  — 和请求路径完全相等
 *      例: "/api/status"
 *      GET /api/status   → 匹配
 *      GET /api/status/  → 不匹配
 *      GET /api/other    → 不匹配
 *
 *   2. Dynamic（动态路由）— 含 <name> 占位符，捕获路径段
 *      例: "/user/<id>"
 *      GET /user/42  → 匹配, params["id"] = "42"
 *      GET /user/abc → 匹配, params["id"] = "abc"
 *
 *      支持类型注解（类似 Flask <type:name>，预留，暂不校验）:
 *        "/user/<int:id>"     id 按字符串提取（不校验整数）
 *        "/user/<string:name>" 等价于 "/user/<name>"
 *
 *   3. Path（路径通配）  — 含 <path:name>，匹配剩余全部路径段（包括 /）
 *      例: "/static/<path:file>"
 *      GET /static/css/app.css    → 匹配, params["file"] = "css/app.css"
 *      GET /static/js/main.js     → 匹配, params["file"] = "js/main.js"
 *
 *      常用: "/<path:path>" 作为 catch-all 兜底路由
 *
 * 匹配优先级: Exact > Dynamic > Path（同优先级按注册顺序）
 */
class JZHttpRoute
{
public:
    JZHttpRoute();
    ~JZHttpRoute();

    // 注册路由，path 语法见上方文档
    void addRoute(const std::string& path, JZHttpHandler* handler);

    // 匹配路径，返回 handler 和路径参数，未匹配返回 false
    bool match(const std::string& path, JZHttpHandler*& handler, std::map<std::string, std::string>& params) const;

    void clear();

private:
    enum RouteType { Exact, Dynamic, Path };

    struct RoutePattern
    {
        RouteType type;
        std::string pattern;
        std::vector<std::string> paramNames;
        std::vector<std::string> segments;
        JZHttpHandler* handler = nullptr;
        int priority;
        int order;
    };

    RouteType parseRouteType(const std::string& path) const;
    void parseRoutePattern(const std::string& path, RoutePattern& pattern) const;
    bool matchExact(const RoutePattern& pattern, const std::string& path) const;
    bool matchDynamic(const RoutePattern& pattern, const std::string& path, std::map<std::string, std::string>& params) const;
    bool matchPath(const RoutePattern& pattern, const std::string& path, std::map<std::string, std::string>& params) const;

    std::vector<RoutePattern> m_routes;
    int m_orderCounter;
};

#endif // JZ_HTTP_ROUTE_H_
