#ifndef JZ_NET_WORK_ACCESS_MANAGER_H_
#define JZ_NET_WORK_ACCESS_MANAGER_H_

#include "JZNetObject.h"
#include "JZByteArray.h"
#include "JZNetClient.h"
#include "JZNetworkRequest.h"
#include "JZNetworkReply.h"
#include "JZOptional.h"

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

    std::string buildConnectRequest(); // 构造发给代理的 CONNECT 请求

    std::string m_url;
    std::string m_hostKey;    // 连接池 key：scheme://host:port（按主机复用）

    JZByteArray m_chunkBuffer;
    JZNetworkReply *m_reply;
    JZNetworkAccessManager* m_manager;
    
    int m_timeId;
    int64_t m_recvTime;
    bool m_chunkMode;
    bool m_closing;           // 正在关闭/已标记回收，禁止再被复用

    // HTTPS 经 HTTP 代理的 CONNECT 隧道状态
    enum ProxyPhase { ProxyPhaseNone = 0, ProxyPhaseConnectSent, ProxyPhaseTunnelUp };
    
    std::string m_targetHost;          // 真实目标主机（SNI / CONNECT 用）
    int m_targetPort = 443;            // 真实目标端口
    std::string m_proxyAuth;           // 代理认证头值 "Basic ..."（用于 CONNECT）
    ProxyPhase m_proxyPhase = ProxyPhaseNone;
    bool m_isHttpsProxy = false;   // 本连接为 https 经 http 代理的隧道
    JZByteArray m_httpsProxyBuffer;
};

// HTTP 代理配置：对 http 与 https 目标均生效。
//  - http  目标：连接到代理地址，并在请求行使用绝对 URL（由 JZNetworkRequest 生成），
//    代理据此把请求转发到真实主机；响应原样返回。
//  - https 目标：连接到代理地址后，先发送 CONNECT host:port 建立隧道，再在隧道上做 TLS 握手，
//    之后发送正常的 https 请求；代理仅盲转发加密流量。
struct JZProxyConfig
{
    std::string host;         // 代理主机（如 "127.0.0.1"）
    int port = 0;             // 代理端口
    std::string user;         // 代理认证用户名（可选）
    std::string password;     // 代理认证密码（可选）

    // 从代理 URL 解析: http://[user:pass@]host:port（scheme 与 userinfo 均可选）
    // 解析失败返回 host 为空的对象（此时视为未配置代理）
    static JZProxyConfig fromUrl(std::string url);
};

class JZNetworkAccessManager : public JZObject
{
public:
    JZNetworkAccessManager(JZObject *parent = nullptr);
    ~JZNetworkAccessManager();

    JZNetworkReply *get(const JZNetworkRequest &request);
    JZNetworkReply *post(const JZNetworkRequest &request, const JZByteArray &buffer);

    // 设置 HTTP 代理（对 http / https 目标均生效；传空 host 的配置即视为关闭）
    void setProxy(const JZProxyConfig &proxy);
    const JZOptional<JZProxyConfig> &proxy() const;

    // 连接池管理
    void setMaxConnectionsPerHost(int n);
    int maxConnectionsPerHost() const;
    void setConnectionIdleTimeout(int64_t ms);
    int64_t connectionIdleTimeout() const;

    // 统一设置 HTTPS 客户端 TLS 配置(需在发起请求前设置)
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
    JZOptional<JZProxyConfig> m_proxyConfig;   // HTTP 代理（http / https 目标均生效；空 = 不启用）
};


#endif // !JZ_NET_WORK_ACCESS_MANAGER_H_
