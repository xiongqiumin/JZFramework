#include "JZNetworkRequest.h"

struct UrlParts {
    std::string host;       
    uint16_t port;          
    std::string path;       
    bool is_https;          
};

UrlParts parseUrl(const std::string& full_url) 
{
    UrlParts parts;
    parts.is_https = false;
    parts.port = 80; 

    size_t proto_end = full_url.find("://");
    if (proto_end == std::string::npos) {
        return UrlParts();
    }
    std::string proto = full_url.substr(0, proto_end);
    if (proto == "https") {
        parts.is_https = true;
        parts.port = 443;
    }
    else if (proto != "http") {
        return UrlParts();
    }

    std::string rest = full_url.substr(proto_end + 3);

    size_t path_start = rest.find('/');
    std::string host_port_str = rest;
    if (path_start != std::string::npos) {
        host_port_str = rest.substr(0, path_start);
        parts.path = rest.substr(path_start);
    }
    else {
        parts.path = "/"; 
    }

    size_t port_start = host_port_str.find(':');
    if (port_start != std::string::npos) {
        parts.host = host_port_str.substr(0, port_start);
        parts.port = static_cast<uint16_t>(std::stoi(host_port_str.substr(port_start + 1)));
    }
    else {
        parts.host = host_port_str;
    }

    return parts;
}

//JZNetworkRequest
JZNetworkRequest::JZNetworkRequest()
{
}

JZNetworkRequest::JZNetworkRequest(const std::string &url)
{
    m_url = url;
}

JZNetworkRequest::JZNetworkRequest(const JZUrl &url)
{
    m_url = url.url();
}

std::string JZNetworkRequest::url() const 
{ 
    return m_url; 
}

std::string JZNetworkRequest::hostKey() const
{
    UrlParts parts = parseUrl(m_url);
    std::string scheme = parts.is_https ? "https" : "http";
    std::string key = scheme + "://" + parts.host;
    if ((parts.is_https && parts.port != 443) || (!parts.is_https && parts.port != 80))
        key += ":" + std::to_string(parts.port);
    return key;
}
    
void JZNetworkRequest::setUrl(const std::string& url) 
{ 
    m_url = url; 
}
    
void JZNetworkRequest::setRawHeader(const std::string& headerName, const std::string& headerValue)
{
    m_headers[headerName] = headerValue;
}

const std::map<std::string, std::string>& JZNetworkRequest::rawHeaders() const
{
    return m_headers;
}
    
void JZNetworkRequest::setHeader(KnownHeaders header, const std::string &headerValue)
{
    switch (header) {
        case ContentTypeHeader:
            setRawHeader("Content-Type", headerValue);
            break;
        case ContentLengthHeader:
            setRawHeader("Content-Length", headerValue);
            break;
        case AuthorizationHeader:
            setRawHeader("Authorization", headerValue);
            break;
        case UserAgentHeader:
            setRawHeader("User-Agent", headerValue);
            break;
        case ConnectionHeader:
            setRawHeader("Connection", headerValue);
            break;
        case CookieHeader:
            setRawHeader("Cookie", headerValue);
            break;
        case HostHeader:
            setRawHeader("Host", headerValue);
            break;
        case AcceptHeader:
            setRawHeader("Accept", headerValue);
            break;
        default: {
            JZ_ASSERT(false);
        }
    }
}
    
std::string JZNetworkRequest::buildHttpHeader(HttpMethod method) const
{
    std::string result;

    UrlParts url_parts = parseUrl(m_url);

    if (method == Get) {
        result += "GET " + url_parts.path + " HTTP/1.1\r\n";
    }
    else {
        result += "POST " + url_parts.path + " HTTP/1.1\r\n";
    }

    result += "Host: " + url_parts.host;
    if ((url_parts.is_https && url_parts.port != 443) || (!url_parts.is_https && url_parts.port != 80)) {
        result += ":" + std::to_string(url_parts.port);
    }
    result += "\r\n";

    for (const auto& pair : m_headers) {
        result += pair.first + ": " + pair.second + "\r\n";
    }

    result += "\r\n";
    return result;
}

std::string JZNetworkRequest::buildWebSocketHeader() const
{
    std::string result;
    for (const auto& pair : m_headers) {
        result += pair.first + ": " + pair.second + "\r\n";
    }
    return result;
}