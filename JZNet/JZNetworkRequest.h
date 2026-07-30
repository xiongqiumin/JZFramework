#ifndef JZ_NETWORK_REQUEST_H_
#define JZ_NETWORK_REQUEST_H_

#include <string>
#include <map>
#include "JZGlobal.h"
#include "JZUrl.h"

class JZNetworkRequest
{
public:
    enum HttpMethod {
        Get,
        Post,
    };

    enum KnownHeaders {
        ContentTypeHeader,
        ContentLengthHeader,
        AuthorizationHeader,
        UserAgentHeader,
        ConnectionHeader,
        CookieHeader,
        HostHeader,
        AcceptHeader,
    };    

    JZNetworkRequest();
    JZNetworkRequest(const std::string& url);
    JZNetworkRequest(const JZUrl &url);

    const JZUrl& url() const;
    void setUrl(const JZUrl &url);

    void setHeader(KnownHeaders header, const std::string &headerValue);
    void setRawHeader(const std::string &headerName, const std::string &headerValue);
    const std::map<std::string, std::string> &rawHeaders() const;

    std::string buildHttpHeader(HttpMethod method) const;
    std::string buildWebSocketHeader() const;

protected:
    JZUrl m_url;
    std::map<std::string,std::string> m_headers;
};

#endif // !JZ_NETWORK_REQUEST_H_