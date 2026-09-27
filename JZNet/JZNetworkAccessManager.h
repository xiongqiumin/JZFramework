#ifndef JZ_NET_WORK_ACCESS_MANAGER_H_
#define JZ_NET_WORK_ACCESS_MANAGER_H_
#include "JZNetObject.h"
#include "JZByteArray.h"
#include "JZNetClient.h"
#include "JZNetworkRequest.h"
#include "JZNetworkReply.h"
#include "JZOptional.h"
#ifdef HAS_JZCompress
#include "JZCompress.h"
#endif

class JZFRAMEWORK_API JZNetworkConnect : public JZNetObject
{
public:
    JZNetworkConnect(JZNetworkAccessManager *manager);
    ~JZNetworkConnect();

jz_signals:
    JZSigVoid sigDisconnected;

protected:
    friend JZNetworkAccessManager;
    friend JZNetworkReply;

    enum JZKeepAlive
    {
        KeepAliveAuto,
        KeepAliveOff,
    };

    virtual void onNetEvent(JZNetResEvent* event) override;
    virtual void timerEvent(JZTimerEvent *event) override;
    void emitFinish(JZNetworkReply::NetworkError error);

    std::string url();
    void open(const std::string& url);
    void reopen();
    void close();
    
    JZByteArray getHttpBody(const JZByteArray& data);
    void appendChunk(const JZByteArray& data);

    // 内部辅助：按 Content-Encoding 解压（作为成员以便访问 JZNetworkReply 的 protected 头字段）
#ifdef HAS_JZCompress
    static bool jzApplyContentEncoding(JZNetworkReply *reply, JZByteArray& body);
#endif

    void resetConnection();   // keep-alive：重置状态以便复用连接
    void closeConnection();   // 真正关闭底层连接

    void updateRequestKeepAlive(const JZNetworkRequest &request);
    std::string buildConnectRequest(); // 构造发给代理的 CONNECT 请求

    std::string m_url;
    std::string m_hostKey;    // 连接池 key：区分直连 / http代理转发 / https代理隧道（按物理连接模式复用）

    JZByteArray m_chunkBuffer;
    JZNetworkReply *m_reply;
    JZNetworkAccessManager* m_manager;
    JZByteArray m_buffer;             // 统一缓冲：代理 CONNECT 响应 / 普通响应头+body / chunked 解压后的压缩体（按阶段复用）
    bool m_responseActive = false;     // 无 Content-Length 且非 chunk 的分段响应：等连接关闭收尾

    int m_timeId;
    int64_t m_recvTime;
    bool m_chunkMode;
        
    // Content-Encoding 为 gzip/x-gzip/deflate：解析响应头时确定算法；appendChunk 据此惰性创建
    // JZDecompressStream，每收到一块就增量 write() 解压，避免整段缓冲 + 收尾一次性解压。
    bool m_compressed = false;                              // 本响应是否为受支持的压缩编码
#ifdef HAS_JZCompress
    JZCompressAlgorithm m_compressAlgo = JZCompressAlgorithm::GZip;  // 配合 m_compressed
    JZDecompressStream *m_decompress = nullptr;             // 流式解压器（惰性创建，响应结束/复用/出错时释放）
#endif
    bool m_closing;           // 正在关闭/已标记回收，禁止再被复用
    // 本连接是否保持复用: 由请求意图(m_keepAlive, 来自请求 Connection 头)与响应
    // Connection 头(m_respClose)共同决定(见 shouldCloseConnection)。
    JZKeepAlive m_keepAlive = KeepAliveAuto;   // 请求声明的复用意图(KeepAliveAuto=由响应决定)
    bool m_respClose = false;                  // 解析服务器响应 Connection 头得到: 响应要求关闭

    // 综合请求意图与响应头, 判定响应结束后是否应关闭连接(不入库复用)
    bool shouldCloseConnection() const;

    // HTTPS 经 HTTP 代理的 CONNECT 隧道状态
    enum ProxyPhase { ProxyPhaseNone = 0, ProxyPhaseConnectSent, ProxyPhaseTunnelUp };    

    std::string m_targetHost;          // 真实目标主机（SNI / CONNECT 用）
    int m_targetPort = 443;            // 真实目标端口
    std::string m_proxyAuth;           // 代理认证头值 "Basic ..."（用于 CONNECT）
    ProxyPhase m_proxyPhase = ProxyPhaseNone;
    bool m_isHttpsProxy = false;   // 本连接为 https 经 http 代理的隧道    
};

// HTTP 代理配置：对 http 与 https 目标均生效。
//  - http  目标：连接到代理地址，并在请求行使用绝对 URL（由 JZNetworkRequest 生成），
//    代理据此把请求转发到真实主机；响应原样返回。
//  - https 目标：连接到代理地址后，先发送 CONNECT host:port 建立隧道，再在隧道上做 TLS 握手，
//    之后发送正常的 https 请求；代理仅盲转发加密流量。
struct JZFRAMEWORK_API JZProxyConfig
{
    std::string host;         // 代理主机（如 "127.0.0.1"）
    int port = 0;             // 代理端口
    std::string user;         // 代理认证用户名（可选）
    std::string password;     // 代理认证密码（可选）

    // 从代理 URL 解析: http://[user:pass@]host:port（scheme 与 userinfo 均可选）
    // 解析失败返回 host 为空的对象（此时视为未配置代理）
    static JZProxyConfig fromUrl(std::string url);
};

class JZFRAMEWORK_API JZNetworkAccessManager : public JZObject
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
