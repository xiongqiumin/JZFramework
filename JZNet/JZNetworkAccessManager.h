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
    uint64_t m_conn;

    JZByteArray m_chunkBuffer;
    JZNetworkReply *m_reply;
    JZNetworkAccessManager* m_manager;
    
    int m_timeId;
    int64_t m_recvTime;
    bool m_chunkMode;
    bool m_closing;           // 正在关闭/已标记回收，禁止再被复用
};

class JZNetworkAccessManager : public JZObject
{
public:
    JZNetworkAccessManager(JZObject *parent = nullptr);
    ~JZNetworkAccessManager();

    JZNetworkReply *get(const JZNetworkRequest &request);
    JZNetworkReply *post(const JZNetworkRequest &request, const JZByteArray &buffer);

    // 连接池管理
    void setMaxConnectionsPerHost(int n);
    int maxConnectionsPerHost() const;
    void setConnectionIdleTimeout(int64_t ms);
    int64_t connectionIdleTimeout() const;

protected:    
    friend JZNetworkConnect;

    JZNetworkConnect* findIdleConnection(const std::string &hostKey);
    void evictIdleConnection(const std::string &hostKey);
    int connectionCount(const std::string &hostKey);

    void open(JZNetworkReply *reply, const JZNetworkRequest &request);
    void onDisconnected();

    // 重定向：返回 true 表示已发起重定向请求（未结束 reply）
    bool followRedirect(JZNetworkReply *reply, const std::string &location);

    std::vector<JZNetworkConnect*> m_netConnects;

    int m_maxConnectionsPerHost;     // 每个主机最大连接数
    int64_t m_connectionIdleTimeoutMs; // 空闲连接回收超时
};


#endif // !JZ_NET_WORK_ACCESS_MANAGER_H_
