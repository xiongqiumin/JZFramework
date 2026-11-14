#include "JZNetworkRequest.h"
#include "JZStd.h"

//JZNetworkRequest
JZNetworkRequest::JZNetworkRequest()
{
}

JZNetworkRequest::JZNetworkRequest(const JZUrl &url)
{
    m_url = url;
}

JZNetworkRequest::JZNetworkRequest(const std::string& url)
{
    m_url.setUrl(url);
}

const JZUrl& JZNetworkRequest::url() const
{
    return m_url;
}

void JZNetworkRequest::setUrl(const JZUrl& url)
{ 
    m_url = url; 
}
    
void JZNetworkRequest::setRawHeader(const std::string& headerName, const std::string& headerValue)
{
    m_headers[headerName] = headerValue;
}

void JZNetworkRequest::removeRawHeader(const std::string& headerName)
{
    std::string lower = str_toLower(headerName);
    for (auto it = m_headers.begin(); it != m_headers.end();)
    {
        if (str_toLower(it->first) == lower)
            it = m_headers.erase(it);
        else
            ++it;
    }
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
        case AcceptEncodingHeader:
            setRawHeader("Accept-Encoding", headerValue);
            break;
        default: {
            JZ_ASSERT(false);
        }
    }
}
    
std::string JZNetworkRequest::buildHttpHeader(HttpMethod method, bool absoluteUri) const
{
    std::string result;

    std::string scheme = m_url.scheme();
    std::string target = absoluteUri ? m_url.url() : (m_url.path() + m_url.query().toString());

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