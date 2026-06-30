#include "JZHttpRoute.h"
#include <algorithm>
#include <sstream>

JZHttpRoute::JZHttpRoute()
    : m_orderCounter(0)
{
}

JZHttpRoute::~JZHttpRoute()
{
}

JZHttpRoute::RouteType JZHttpRoute::parseRouteType(const std::string& path) const
{
    if (path.empty())
        return Exact;

    bool hasDynamic = false;
    bool hasPath = false;

    size_t pos = 0;
    while (pos < path.length())
    {
        if (path[pos] == '<')
        {
            size_t end = path.find('>', pos);
            if (end != std::string::npos)
            {
                std::string param = path.substr(pos + 1, end - pos - 1);
                
                size_t colon = param.find(':');
                if (colon != std::string::npos)
                {
                    std::string type = param.substr(colon + 1);
                    if (type == "path")
                    {
                        hasPath = true;
                    }
                    else
                    {
                        hasDynamic = true;
                    }
                }
                else
                {
                    hasDynamic = true;
                }
                
                pos = end + 1;
            }
            else
            {
                pos++;
            }
        }
        else
        {
            pos++;
        }
    }

    if (hasPath)
        return Path;
    else if (hasDynamic)
        return Dynamic;
    else
        return Exact;
}

void JZHttpRoute::parseRoutePattern(const std::string& path, RoutePattern& pattern) const
{
    pattern.pattern = path;
    pattern.type = parseRouteType(path);
    pattern.paramNames.clear();
    pattern.segments.clear();

    if (pattern.type == Exact)
    {
        pattern.segments.push_back(path);
        return;
    }

    std::string current;
    size_t pos = 0;

    while (pos < path.length())
    {
        if (path[pos] == '<')
        {
            if (!current.empty())
            {
                pattern.segments.push_back(current);
                current.clear();
            }

            size_t end = path.find('>', pos);
            if (end != std::string::npos)
            {
                std::string param = path.substr(pos + 1, end - pos - 1);
                
                size_t colon = param.find(':');
                if (colon != std::string::npos)
                {
                    std::string name = param.substr(0, colon);
                    std::string type = param.substr(colon + 1);
                    pattern.paramNames.push_back(name);
                    pattern.segments.push_back("<" + type + ">");
                }
                else
                {
                    pattern.paramNames.push_back(param);
                    pattern.segments.push_back("<>");
                }
                
                pos = end + 1;
            }
            else
            {
                current += path[pos];
                pos++;
            }
        }
        else
        {
            current += path[pos];
            pos++;
        }
    }

    if (!current.empty())
    {
        pattern.segments.push_back(current);
    }
}

bool JZHttpRoute::matchExact(const RoutePattern& pattern, const std::string& path) const
{
    return pattern.pattern == path;
}

bool JZHttpRoute::matchDynamic(const RoutePattern& pattern, const std::string& path, std::map<std::string, std::string>& params) const
{
    std::string patternPath = pattern.pattern;
    std::string requestPath = path;

    size_t paramIndex = 0;
    size_t patternPos = 0;
    size_t requestPos = 0;

    while (patternPos < patternPath.length() && requestPos < requestPath.length())
    {
        if (patternPath[patternPos] == '<')
        {
            size_t end = patternPath.find('>', patternPos);
            if (end == std::string::npos)
                return false;

            std::string param = patternPath.substr(patternPos + 1, end - patternPos - 1);
            std::string paramName = param;
            
            size_t colon = param.find(':');
            if (colon != std::string::npos)
            {
                paramName = param.substr(0, colon);
            }

            std::string nextSegment;
            size_t nextStart = end + 1;
            size_t slashPos = patternPath.find('/', nextStart);
            if (slashPos != std::string::npos)
            {
                nextSegment = patternPath.substr(nextStart, slashPos - nextStart);
            }

            std::string paramValue;
            if (!nextSegment.empty())
            {
                size_t nextPos = requestPath.find(nextSegment, requestPos);
                if (nextPos == std::string::npos)
                    return false;
                
                paramValue = requestPath.substr(requestPos, nextPos - requestPos);
                requestPos = nextPos;
            }
            else
            {
                paramValue = requestPath.substr(requestPos);
                requestPos = requestPath.length();
            }

            if (paramIndex < pattern.paramNames.size())
            {
                params[pattern.paramNames[paramIndex]] = paramValue;
            }
            paramIndex++;

            patternPos = end + 1;
        }
        else if (patternPath[patternPos] == requestPath[requestPos])
        {
            patternPos++;
            requestPos++;
        }
        else
        {
            return false;
        }
    }

    return patternPos == patternPath.length() && requestPos == requestPath.length();
}

bool JZHttpRoute::matchPath(const RoutePattern& pattern, const std::string& path, std::map<std::string, std::string>& params) const
{
    std::string patternPath = pattern.pattern;
    std::string requestPath = path;

    size_t pathParamPos = patternPath.find("<path:");
    if (pathParamPos == std::string::npos)
        return false;

    size_t pathParamEnd = patternPath.find('>', pathParamPos);
    if (pathParamEnd == std::string::npos)
        return false;

    std::string paramName = patternPath.substr(pathParamPos + 5, pathParamEnd - pathParamPos - 5);

    std::string prefix = patternPath.substr(0, pathParamPos);
    std::string suffix = patternPath.substr(pathParamEnd + 1);

    if (!prefix.empty() && requestPath.find(prefix) != 0)
        return false;

    if (!suffix.empty() && requestPath.rfind(suffix) != requestPath.length() - suffix.length())
        return false;

    std::string pathValue;
    if (!prefix.empty())
    {
        pathValue = requestPath.substr(prefix.length());
    }
    else
    {
        pathValue = requestPath;
    }

    if (!suffix.empty() && pathValue.length() >= suffix.length())
    {
        pathValue = pathValue.substr(0, pathValue.length() - suffix.length());
    }

    params[paramName] = pathValue;

    return true;
}

void JZHttpRoute::addRoute(const std::string& path, JZHttpHandler* handler)
{
    RoutePattern pattern;
    parseRoutePattern(path, pattern);
    pattern.handler = handler;
    pattern.order = m_orderCounter++;
    switch (pattern.type) {
        case Exact:    pattern.priority = 3; break;
        case Dynamic:  pattern.priority = 2; break;
        case Path:     pattern.priority = 1; break;
    }
    m_routes.push_back(pattern);
}

bool JZHttpRoute::match(const std::string& path, JZHttpHandler*& handler, std::map<std::string, std::string>& params) const
{
    params.clear();

    auto compareRoutes = [](const RoutePattern& a, const RoutePattern& b) {
        if (a.priority != b.priority)
            return a.priority > b.priority;
        return a.order < b.order;
    };

    std::vector<RoutePattern> sortedRoutes = m_routes;
    std::sort(sortedRoutes.begin(), sortedRoutes.end(), compareRoutes);

    for (const RoutePattern& pattern : sortedRoutes)
    {
        bool matched = false;
        switch (pattern.type)
        {
            case Exact:
                matched = matchExact(pattern, path);
                break;
            case Dynamic:
                matched = matchDynamic(pattern, path, params);
                break;
            case Path:
                matched = matchPath(pattern, path, params);
                break;
        }

        if (matched)
        {
            handler = pattern.handler;
            return true;
        }
        else
        {
            params.clear();
        }
    }

    return false;
}

void JZHttpRoute::clear()
{
    m_routes.clear();
    m_orderCounter = 0;
}
