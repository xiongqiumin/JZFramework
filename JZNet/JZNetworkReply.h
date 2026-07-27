#ifndef JZ_NEWWORK_REPLY_H_
#define JZ_NEWWORK_REPLY_H_

#include "JZNetObject.h"
#include "JZByteArray.h"
#include "JZNetworkRequest.h"

class JZNetworkAccessManager;

// HTTP response data
class JZHttpResData : public JZNetResEventData
{
public:
    int httpCode;
    std::string httpCodeText;
};

class JZNetworkConnect;
class JZNetworkReply : public JZObject
{
public:
    enum NetworkError {
        None,
        NoError,
        ConnectError,
        HttpError,
        TimeOutError,
        UserCancelError,
    };

    enum RedirectPolicy {
        NoRedirect,          // 不跟随
        SameOrigin,          // 仅同源（默认推荐）
        SameHost,            // 同主机（不同端口/协议也跟随）
        AllAllowed           // 允许所有（需谨慎）
    };

    JZNetworkReply(JZObject*object);
    ~JZNetworkReply();    

    void close();

    int64_t timeout() const;
    void setTimeOut(int64_t timeout);

    NetworkError error() const;    
    std::string errorString() const;

    int httpStatusCode() const;
    std::string httpStatusCodeText() const;
    bool isFinished() const;

    std::string rawHeaders() const;   // 原始响应头（head）
    const std::map<std::string, std::string>& headers() const;  // 解析后的头（key 已小写）
    std::string header(const std::string &key) const;            // 按 key 取单个头（大小写不敏感）

    // 重定向支持
    void setFollowRedirects(bool enable);
    bool isFollowRedirects() const;
    void setRedirectPolicy(RedirectPolicy policy);
    RedirectPolicy redirectPolicy() const;
    void setMaxRedirects(int max);
    int maxRedirects() const;
    int redirectCount() const;

    JZByteArray readAll();

jz_signals:
    JZSigVoid sigReadyRead;
    JZSigVoid sigFinished;

protected:    
    friend JZNetworkAccessManager;
    friend JZNetworkConnect;

    void emitFinish(NetworkError error);

    NetworkError m_error;

    JZByteArray m_send;
    JZByteArray m_recv;
    JZByteArray m_body;

    int m_statusCode;
    std::string m_statusText;
    std::string m_head;           // 原始响应头（head）
    std::map<std::string, std::string> m_headers;  // 解析后的头（key 已小写）

    JZNetworkRequest m_request;
    JZNetworkConnect *m_conn;
    int m_timeOut;

    // 重定向支持
    RedirectPolicy m_redirectPolicy;
    int m_maxRedirects;
    int m_redirectCount;
    JZNetworkRequest::HttpMethod m_method;   // 原始请求方法，用于重定向时重建
    JZByteArray m_postBody;                  // POST body，用于重定向时重建
};

#endif // !JZ_NEWWORK_REPLY_H_