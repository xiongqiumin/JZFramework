#include "JZHttpRequest.h"
#include <algorithm>
#include <sstream>

JZHttpRequest::JZHttpRequest()
    : m_method(UNKNOWN)
{
}

JZHttpRequest::~JZHttpRequest()
{
}

JZHttpRequest::Method JZHttpRequest::method() const
{
    return m_method;
}

void JZHttpRequest::setMethod(Method method)
{
    m_method = method;
    m_methodString = methodToString(method);
}

const std::string& JZHttpRequest::methodString() const
{
    return m_methodString;
}

void JZHttpRequest::setMethodString(const std::string& methodString)
{
    m_methodString = methodString;
    m_method = stringToMethod(methodString);
}

const std::string& JZHttpRequest::path() const
{
    return m_path;
}

void JZHttpRequest::setPath(const std::string& path)
{
    m_path = path;
}

const std::string& JZHttpRequest::query() const
{
    return m_query;
}

void JZHttpRequest::setQuery(const std::string& query)
{
    m_query = query;
}

const std::string& JZHttpRequest::header(const std::string& key) const
{
    static const std::string empty;
    auto it = m_headers.find(key);
    if (it != m_headers.end())
        return it->second;
    return empty;
}

void JZHttpRequest::setHeader(const std::string& key, const std::string& value)
{
    m_headers[key] = value;
}

const std::map<std::string, std::string>& JZHttpRequest::headers() const
{
    return m_headers;
}

const std::string& JZHttpRequest::param(const std::string& key) const
{
    static const std::string empty;
    auto it = m_params.find(key);
    if (it != m_params.end())
        return it->second;
    return empty;
}

void JZHttpRequest::setParam(const std::string& key, const std::string& value)
{
    m_params[key] = value;
}

const std::map<std::string, std::string>& JZHttpRequest::params() const
{
    return m_params;
}

const std::string& JZHttpRequest::queryParam(const std::string& key) const
{
    static const std::string empty;
    auto it = m_queryParams.find(key);
    if (it != m_queryParams.end())
        return it->second;
    return empty;
}

void JZHttpRequest::setQueryParam(const std::string& key, const std::string& value)
{
    m_queryParams[key] = value;
}

const std::map<std::string, std::string>& JZHttpRequest::queryParams() const
{
    return m_queryParams;
}

const JZByteArray& JZHttpRequest::body() const
{
    return m_body;
}

void JZHttpRequest::setBody(const JZByteArray& body)
{
    m_body = body;
}

const std::string& JZHttpRequest::contentType() const
{
    return header("Content-Type");
}

const std::string& JZHttpRequest::userAgent() const
{
    return header("User-Agent");
}

const std::string& JZHttpRequest::remoteAddr() const
{
    return m_remoteAddr;
}

void JZHttpRequest::setRemoteAddr(const std::string& addr)
{
    m_remoteAddr = addr;
}

JZHttpRequest::Method JZHttpRequest::stringToMethod(const std::string& method)
{
    std::string upperMethod = method;
    std::transform(upperMethod.begin(), upperMethod.end(), upperMethod.begin(), ::toupper);

    if (upperMethod == "GET")
        return GET;
    else if (upperMethod == "POST")
        return POST;
    else if (upperMethod == "PUT")
        return PUT;
    else if (upperMethod == "DELETE")
        return DELETE;
    else if (upperMethod == "HEAD")
        return HEAD;
    else if (upperMethod == "OPTIONS")
        return OPTIONS;
    else if (upperMethod == "PATCH")
        return PATCH;
    else
        return UNKNOWN;
}

std::string JZHttpRequest::methodToString(Method method)
{
    switch (method)
    {
        case GET: return "GET";
        case POST: return "POST";
        case PUT: return "PUT";
        case DELETE: return "DELETE";
        case HEAD: return "HEAD";
        case OPTIONS: return "OPTIONS";
        case PATCH: return "PATCH";
        default: return "UNKNOWN";
    }
}
