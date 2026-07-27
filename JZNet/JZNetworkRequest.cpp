#include "JZNetworkRequest.h"

//JZNetworkRequest
JZNetworkRequest::JZNetworkRequest()
{
}

JZNetworkRequest::JZNetworkRequest(const std::string &url)
{
    m_url.setUrl(url);
}

JZNetworkRequest::JZNetworkRequest(const JZUrl &url)
{
    m_url = url;
}

std::string JZNetworkRequest::url() const 
{ 
    return m_url.url(); 
}

std::string JZNetworkRequest::hostKey() const
{
    std::string scheme = m_url.scheme();
    std::string key = scheme + "://" + m_url.host();
    if ((scheme == "https" && m_url.port() != 443) || (scheme == "http" && m_url.port() != 80))
        key += ":" + std::to_string(m_url.port());
    return key;
}
    
void JZNetworkRequest::setUrl(const std::string& url) 
{ 
    m_url.setUrl(url); 
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

    std::string scheme = m_url.scheme();
    std::string target = m_url.path() + m_url.query().toString();

    if (method == Get) {
        result += "GET " + target + " HTTP/1.1\r\n";
    }
    else {
        result += "POST " + target + " HTTP/1.1\r\n";
    }

    result += "Host: " + m_url.host();
    if ((scheme == "https" && m_url.port() != 443) || (scheme == "http" && m_url.port() != 80)) {
        result += ":" + std::to_string(m_url.port());
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