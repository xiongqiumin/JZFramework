#ifndef JZ_HTTP_ROUTE_H_
#define JZ_HTTP_ROUTE_H_

#include <string>
#include <vector>
#include <map>
#include <functional>
#include "JZHttpRequest.h"
#include "JZHttpRespone.h"
#include "JZHttpHandler.h"

class JZHttpRoute
{
public:
    JZHttpRoute();
    ~JZHttpRoute();

    void addRoute(const std::string& path, JZHttpHandler* handler);
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
