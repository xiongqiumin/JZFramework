#ifndef JZ_HTTP_REQUEST_H_
#define JZ_HTTP_REQUEST_H_
#include <string>
#include <map>
#include "JZByteArray.h"

class JZFRAMEWORK_API JZHttpRequest
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

    std::string query() const;
    // 设置原始 query 串(如 "a=1&b=2")。内部会立即解析并**覆盖**整个参数表,
    // 之后可直接用 queryParam()/queryParams() 取参数(值已做 URL 解码)。
    void setQuery(const std::string& query);

    const std::string& header(const std::string& key) const;
    // 设置请求头。key 为 Cookie 时会同步解析到 cookie()/cookies()（多次设置按累加处理）。
    void setHeader(const std::string& key, const std::string& value);
    const std::map<std::string, std::string>& headers() const;

    // 解析后的 Cookie(键为 cookie 名, 值已去掉两端双引号)。没有 Cookie 头时为空表。
    const std::string& cookie(const std::string& name) const;
    const std::map<std::string, std::string>& cookies() const;

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
    std::map<std::string, std::string> m_headers;
    std::map<std::string, std::string> m_cookies;
    std::map<std::string, std::string> m_params;
    std::map<std::string, std::string> m_queryParams;
    JZByteArray m_body;
    std::string m_remoteAddr;
};

#endif // !JZ_HTTP_REQUEST_H_
