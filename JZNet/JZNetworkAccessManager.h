#ifndef JZ_NET_WORK_ACCESS_MANAGER_H_
#define JZ_NET_WORK_ACCESS_MANAGER_H_

#include "JZNetObject.h"
#include "JZByteArray.h"
#include "JZNetClient.h"
#include "JZNetworkRequest.h"
#include "JZNetworkReply.h"

class JZNetworkConnect : public JZNetObject
{
public:
    JZNetworkConnect(JZNetworkAccessManager *manager);
    ~JZNetworkConnect();

jz_signals:
    JZSigVoid sigDisconnected;

protected:
    friend JZNetworkAccessManager;
    friend JZNetworkReply;

    virtual void onNetEvent(JZNetResEvent* event) override;
    virtual void timerEvent(JZTimerEvent *event) override;
    void emitFinish(JZNetworkReply::NetworkError error);

    std::string url();
    void open(const std::string& url);
    void reopen();
    void close();
    
    JZByteArray getHttpBody(const JZByteArray& data);
    void appendChunk(const JZByteArray& data);

    void resetConnection();   // keep-alive：重置状态以便复用连接
    void closeConnection();   // 真正关闭底层连接

    std::string m_url;
    std::string m_hostKey;    // 连接池 key：scheme://host:port（按主机复用）

    JZByteArray m_chunkBuffer;
    JZNetworkReply *m_reply;
    JZNetworkAccessManager* m_manager;
    
    int m_timeId;
    int64_t m_recvTime;
    bool m_chunkMode;
    bool m_closing;           // 正在关闭/已标记回收，禁止再被复用
};

// HTTP 代理配置：仅对 http 目标生效。
// 工作方式 = 连接到代理地址，并在请求行使用绝对 URL（由 JZNetworkRequest 生成），
// 代理据此把请求转发到真实主机；响应原样返回。
// 注意：https 目标需要 CONNECT 隧道，这里未实现，https 请求会忽略代理直接连接。
struct JZProxyConfig
{
    bool enabled = false;     // 是否启用代理
    std::string host;         // 代理主机（如 "127.0.0.1"）
    int port = 0;             // 代理端口
    std::string user;         // 代理认证用户名（可选）
    std::string password;     // 代理认证密码（可选）
};

class JZNetworkAccessManager : public JZObject
{
public:
    JZNetworkAccessManager(JZObject *parent = nullptr);
    ~JZNetworkAccessManager();

    JZNetworkReply *get(const JZNetworkRequest &request);
    JZNetworkReply *post(const JZNetworkRequest &request, const JZByteArray &buffer);

    // 设置 HTTP 代理（仅对 http 目标生效；传空 host / enabled=false 即关闭）
    void setProxy(const JZProxyConfig &proxy);
    const JZProxyConfig &proxy() const;

    // 连接池管理
    void setMaxConnectionsPerHost(int n);
    int maxConnectionsPerHost() const;
    void setConnectionIdleTimeout(int64_t ms);
    int64_t connectionIdleTimeout() const;

    // 统一设置 HTTPS 客户端 TLS 配置(需在发起请求前设置)
    //  - caPem:        CA 证书(PEM), 用于校验服务端证书; 为空则跳过校验。
    //  - clientCertPem: 客户端证书(PEM), 用于双向认证(可选); 空则无。
    //  - clientKeyPem:  客户端私钥(PEM)。
    //  - skipVerify:    是否跳过服务端证书校验(默认 false)。
    void setSsl(const std::string &caPem,
                const std::string &clientCertPem,
                const std::string &clientKeyPem,
                bool skipVerify = false);
    void setSsl(const JZSSLConfig &ssl);

protected:    
    friend JZNetworkConnect;

    JZNetworkConnect* findIdleConnection(const std::string &hostKey);
    void evictIdleConnection(const std::string &hostKey);
    int connectionCount(const std::string &hostKey);

    std::string hostKey(const JZNetworkRequest& request) const;
    void open(JZNetworkReply *reply, const JZNetworkRequest &request);
    void onDisconnected();

    // 重定向：返回 true 表示已发起重定向请求（未结束 reply）
    bool followRedirect(JZNetworkReply *reply, const std::string &location);

    std::vector<JZNetworkConnect*> m_netConnects;

    int m_maxConnectionsPerHost;     // 每个主机最大连接数
    int64_t m_connectionIdleTimeoutMs; // 空闲连接回收超时

    // HTTPS 客户端 TLS 配置
    JZSSLConfig m_ssl;              // 客户端 TLS 配置
    JZProxyConfig m_proxy;          // HTTP 代理（仅对 http 目标生效）
};


#endif // !JZ_NET_WORK_ACCESS_MANAGER_H_
