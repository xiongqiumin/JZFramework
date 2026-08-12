#ifndef JZ_HTTP_REQUEST_H_
#define JZ_HTTP_REQUEST_H_

#include <string>
#include <map>
#include "JZByteArray.h"

class JZHttpRequest
{
public:
    enum Method
    {
        GET,
        POST,
        PUT,
        DELETE,
        HEAD,
        OPTIONS,
        PATCH,
        UNKNOWN
    };

    JZHttpRequest();
    ~JZHttpRequest();

    Method method() const;
    void setMethod(Method method);

    const std::string& methodString() const;
    void setMethodString(const std::string& method);

    const std::string& path() const;
    void setPath(const std::string& path);

    const std::string& query() const;
    void setQuery(const std::string& query);

    const std::string& header(const std::string& key) const;
    void setHeader(const std::string& key, const std::string& value);
    const std::map<std::string, std::string>& headers() const;

    const std::string& param(const std::string& key) const;
    void setParam(const std::string& key, const std::string& value);
    const std::map<std::string, std::string>& params() const;

    const std::string& queryParam(const std::string& key) const;
    void setQueryParam(const std::string& key, const std::string& value);
    const std::map<std::string, std::string>& queryParams() const;

    const JZByteArray& body() const;
    void setBody(const JZByteArray& body);

    const std::string& contentType() const;
    const std::string& userAgent() const;
    const std::string& remoteAddr() const;
    void setRemoteAddr(const std::string& addr);

    static Method stringToMethod(const std::string& method);
    static std::string methodToString(Method method);

private:
    Method m_method;
    std::string m_methodString;
    std::string m_path;
    std::string m_query;
    std::map<std::string, std::string> m_headers;
    std::map<std::string, std::string> m_params;
    std::map<std::string, std::string> m_queryParams;
    JZByteArray m_body;
    std::string m_remoteAddr;
};

#endif // !JZ_HTTP_REQUEST_H_
