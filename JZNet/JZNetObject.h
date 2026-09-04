#ifndef JZ_NET_OBJECT_H_
#define JZ_NET_OBJECT_H_

#include "JZObject.h"
#include "JZNetPacket.h"
#include "JZThread.h"

class JZNetObject;
struct mg_connection;
struct mg_mgr;

constexpr uint64_t INVALID_NET_HANDLE = 0;

enum JZNetEvent
{
    JZ_NET_EV_ERROR,      // Error                        char *error_message
    JZ_NET_EV_OPEN,       // Connection created           NULL
    JZ_NET_EV_POLL,       // mg_mgr_poll iteration        uint64_t *uptime_millis
    JZ_NET_EV_RESOLVE,    // Host name is resolved        NULL
    JZ_NET_EV_CONNECT,    // Connection established       NULL
    JZ_NET_EV_ACCEPT,     // Connection accepted          NULL
    JZ_NET_EV_TLS_HS,     // TLS handshake succeeded      NULL
    JZ_NET_EV_READ,       // Data received from socket    long *bytes_read
    JZ_NET_EV_WRITE,      // Data written to socket       long *bytes_written
    JZ_NET_EV_CLOSE,      // Connection closed            NULL
    JZ_NET_EV_HTTP_HDRS,  // HTTP headers                 struct mg_http_message *
    JZ_NET_EV_HTTP_MSG,   // Full HTTP request/response   struct mg_http_message *
    JZ_NET_EV_WS_OPEN,    // Websocket handshake done     struct mg_http_message *
    JZ_NET_EV_WS_MSG,     // Websocket msg, text or bin   struct mg_ws_message *
    JZ_NET_EV_WS_CTL,     // Websocket control msg        struct mg_ws_message *
    JZ_NET_EV_MQTT_CMD,   // MQTT low-level command       struct mg_mqtt_message *
    JZ_NET_EV_MQTT_MSG,   // MQTT PUBLISH received        struct mg_mqtt_message *
    JZ_NET_EV_MQTT_OPEN,  // MQTT CONNACK received        int *connack_status_code
    JZ_NET_EV_SNTP_TIME,  // SNTP time received           uint64_t *epoch_millis

    JZ_NET_EV_HTTP_CHUNK_MODE = 256,
};
const char *netEventString(int event);

enum
{
    JZ_WEBSOCKET_OP_CONTINUE = 0,
    JZ_WEBSOCKET_OP_TEXT = 1,
    JZ_WEBSOCKET_OP_BINARY = 2,
    JZ_WEBSOCKET_OP_CLOSE = 8,
    JZ_WEBSOCKET_OP_PING = 9,
    JZ_WEBSOCKET_OP_PONG = 10
};

enum SocketState 
{ 
    UnconnectedState,     
    ConnectingState, 
    ConnectedState
};

enum SocketWrite
{
    WriteBin,
    WriteWebSocketText,
    WriteWebSocketBin,
    WriteWebSocketPing,
    WriteMqttMessage,
    WriteMqttServerResp,
    WriteUdp,
};

// TLS 配置: CA 证书 / 本端证书 / 本端私钥 / 是否跳过校验
struct JZSSLConfig
{
    std::string sslCa;            // CA 证书内容(PEM): 客户端校验服务端 / 服务端双向认证
    std::string sslCert;          // 证书内容(PEM): 服务端=服务器证书 / 客户端=客户端证书
    std::string sslKey;           // 私钥内容(PEM): 与 sslCert 配对
    bool sslSkipVerify = false;   // 客户端是否跳过证书校验(仅客户端角色生效)

    bool isEmpty() const
    {
        return sslCa.empty() && sslCert.empty() && sslKey.empty() && !sslSkipVerify;
    }
};

struct JZSocketInfo
{
    enum ConnType
    {
        None,
        TcpSocket,
        TcpServer,
        UdpSocket,
        UdpListen,
        HttpClient,
        HttpClientHttpsProxy,
        HttpClientChunked, //连接后如果是chunked, net thread接线头改变 
        HttpClientServer,  //服务端的连接
        HttpServer,
        WebSocket,
        WebSocketServer,  //服务端的websocket
        WebSocketListen,  //服务端的侦听
        MqttClient,
        MqttClientServer,  //服务端的mqtt client
        MqttServer,
    };

    ConnType type;
    std::string url;
    std::string websocketExt;
};

class JZNetOpenReq
{
public:
    JZNetOpenReq();

    JZSocketInfo info;
    JZNetObject* object;
    uint64_t conn;       //这里复用 mg_connection 的id, 所以是 uint64_t,这是返回的id 
};

class JZNetReqEventData
{
public:
    virtual ~JZNetReqEventData();
};

class JZNetReqEvent : public JZEvent
{
public:
    enum {
        Net_Open,
        Net_Write,
        Net_Draining,
        Net_HttpClientReopen,
        Net_HttpClientProxyTlsInit,
        Net_HttpReplyFinish,
        Net_Close,
        Net_SetReadLimit,
    };

    JZNetReqEvent();
    ~JZNetReqEvent();

    int netCmd;
    JZNetOpenReq *openReq;
    uint64_t conn;
    JZSemaphore *sem;

    //write
    int writeCmd;
    JZByteArray buffer;

    //ext
    std::shared_ptr<JZNetReqEventData> extData;

    // Net_tlsInit 参数: 在代理隧道上做 TLS 握手时使用的真实目标主机(SNI)
    std::string tlsHost;

    // setReadLimit 参数: 下行接收限速(字节/秒)
    int64_t readLimitBytesPerSec = 0;
};

//JZNetResEvent
class JZNetResEventData
{
public:
    virtual ~JZNetResEventData();
};

class JZNetResEvent : public JZEvent
{
public:
    JZNetResEvent();
    ~JZNetResEvent();

    JZNetEvent event;
    uint64_t conn;
    JZByteArray binData;

    // ext
    std::shared_ptr<JZNetResEventData> extData;
};

class JZNetThread : public JZThread
{
public:
    static JZNetThread *instance();    
        
    void addObject(JZNetObject *object);
    void removeObject(JZNetObject *object);
    void wakeUpNet();

protected:
    struct ConnInfo
    {
        ConnInfo();

        JZSocketInfo::ConnType type;
        JZObjectPointer object;
        mg_connection *c;
        uint64_t acceptId;  //这个conn 是由那个 server accept的, acceptId 和服务 object 关联

        // 下行接收限速(单位 字节/秒, 0=不限速): 在 MG_EV_POLL 里按令牌桶决定 is_full
        int64_t readLimitBytesPerSec = 0;
        int64_t readBudget = 0;   // 当前可用接收预算(字节)
        int64_t readLastMs = 0;   // 上次更新预算的时间(ms)

        // 服务端 TLS 证书(监听连接配置, accept 出的连接据此初始化 TLS)
        JZSSLConfig ssl;
    };

    friend void mg_thread_fn(mg_connection *c, int ev, void *ev_data);

    // HttpClient 流式解析状态(net 线程内持续存在, 跨多次 MG_EV_READ)
    struct JZHttpClientParse
    {
        bool headerDone = false;     // 响应头是否已解析
        int statusCode = 0;          // HTTP 状态码
        bool isSse = false;          // Content-Type: text/event-stream
        bool chunked = false;        // Transfer-Encoding: chunked
        int64_t contentLength = -1;  // Content-Length(未知为 -1)
        int64_t bodyReceived = 0;    // 已上送的 body 字节数
    };

    JZNetThread(JZObject *parent);
    ~JZNetThread();

    virtual void run() override;
    virtual bool event(JZEvent* event) override;
    
    void onFinished();
    void onNetEvent(JZNetReqEvent* event);
    void initTls(mg_connection *conn, const JZSSLConfig &ssl, const std::string &host);
    void bindObject(uint64_t conn, JZNetObject* object);

    // 每隔 5 秒打印一次当前网络连接概况(连接数 / 每个连接的本地远端地址、类型、状态)    
    void printNetConnect();

    mg_mgr *m_mgr;

    std::map<uint64_t, ConnInfo> m_connMap;
    std::vector<JZNetObject*> m_objectList;
    std::map<int, std::vector<int>> m_eventFilter;
    JZMutex m_objectMutex;
};

class JZNetObject : public JZObject
{
    JZ_OBJECT_DECLARE(JZNetObject)

public:    
    JZNetObject(JZObject *parent = nullptr);
    virtual ~JZNetObject();

    int pendingWriteBytes() const;

    // 设置下行接收限速(字节/秒), 0=不限速。内部按令牌桶在 net thread 控制 TCP 接收窗口
    void setReadLimit(int64_t rateBytesPerSec);
    int64_t readLimit() const { return m_readLimitBytesPerSec; }

    void setSslConfig(const JZSSLConfig &ssl);
    const JZSSLConfig &sslConfig() const { return m_ssl; }
    // 便捷接口: 等价于 setSslConfig({caPem, certPem, keyPem, false})
    void setSsl(const std::string &caPem, const std::string &certPem, const std::string &keyPem);

protected:
    // TLS 配置(由 setSsl 设置, 建连时填入 JZSocketInfo); 服务端/客户端共用同一组字段。
    JZSSLConfig m_ssl;

    friend void mg_thread_fn(mg_connection *c, int ev, void *ev_data);

    // 【核心问题】必须将accept核心逻辑放在网络线程执行，原因如下：
    // 1. 网络线程在完成TCP握手后，会立即触发read事件读取WebSocket数据
    // 2. 若将socket对象创建逻辑异步派发至server线程，会存在时序窗口：
    //    - 网络线程已收到数据并触发read事件
    //    - 但server线程尚未完成socket对象的创建与绑定
    // 3. 最终导致read事件处理时找不到对应的socket管理对象，造成数据丢失
    //
    // 【解决方案】
    // - 网络线程内同步完成socket对象创建、连接绑定等核心操作
    // - 仅将"连接建立完成"的通知事件异步发送至server线程（不影响数据处理）
    // - 确保网络线程处理任何IO事件前，socket对象已存在且绑定完成
    // onAccept 返回server 新创建的 JZNetObject, 网络线程会进行绑定 object -> conn 
    virtual JZNetObject *onAccept(uint64_t conn);
    //在net thread移除自身, 需要在一进入析构就调用,避免析构后还在net thread处理事件
    void removeNetObject();

    virtual bool event(JZEvent *event) override;
    virtual void onNetEvent(JZNetResEvent *event) = 0;
    void wakeUpNet();

    void postReqEvent(JZNetReqEvent *event);
    bool openConn(const JZSocketInfo &info, std::shared_ptr<JZNetReqEventData> data = std::shared_ptr<JZNetReqEventData>());
    void closeConn(bool closeImmediate);
    void writeConn(SocketWrite type, const void *buf, int len);

    int64_t m_readLimitBytesPerSec = 0;   // 本对象下行接收限速(字节/秒), 0=不限速
    uint64_t m_conn = INVALID_NET_HANDLE;   // 本对象当前连接 id (1:1)
    JZSocketInfo::ConnType m_connType = JZSocketInfo::None;
    int m_pendingBytes = 0;
};


class JZNetConfig
{
public:
    int netThreadPriority = 6;
    int netThreadCore = -1;    // cpu core

    int maxRecvSize = 3 * 1024 * 1024;  //单个连接最大接收缓冲区
    int maxSendSize = 3 * 1024 * 1024;  //单个连接最大发送缓冲区
    std::string dns4;  // IPv4 DNS 服务器, 为空则不配置
    std::string dns6;  // IPv6 DNS 服务器, 为空则不配置
};

class JZNet
{
public:
    static void setConfig(JZNetConfig config);
};

#endif // !JZ_NET_OBJECT_H_
