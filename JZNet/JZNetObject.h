#ifndef JZ_NET_OBJECT_H_
#define JZ_NET_OBJECT_H_

#include "JZObject.h"
#include "JZNetPacket.h"
#include "JZThread.h"

class JZNetObject;

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
    WriteMqttMessage,
    WriteMqttServerResp,
};

struct JZSocketInfo
{
    enum ConnType
    {
        None,
        TcpSocket,
        TcpServer,
        HttpClient,
        HttpServerClient, //服务端的连接
        HttpServer,
        WebSocket,
        WebServerClient,  //服务端的websocket
        WebSocketListen,  //服务端的侦听
        MqttClient,
        MqttServerClient,  //服务端的mqtt client
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
        Net_HttpReplyFinish,
        Net_Close,
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

    //http
    int httpCode;
    std::string httpCodeText;

    //ws
    int wsFlag;

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
    };

    friend void mg_thread_fn(mg_connection *c, int ev, void *ev_data);

    JZNetThread(JZObject *parent);
    ~JZNetThread();

    virtual void run() override;
    virtual bool event(JZEvent* event) override;
    void onNetEvent(JZNetReqEvent* event);
    void initTls(mg_connection *conn,const std::string &url);
    void bindObject(uint64_t conn, JZNetObject* object);

    mg_mgr *m_mgr;

    std::map<uint64_t, ConnInfo> m_connMap;
    std::vector<JZNetObject*> m_objectList;
    std::map<int, std::vector<int>> m_eventFilter;
    JZMutex m_mutex;
};


class JZNetObject : public JZObject
{
    JZ_OBJECT_DECLARE(JZNetObject)

public:
    JZNetObject(JZObject *parent = nullptr);
    virtual ~JZNetObject();

protected:        
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

    virtual bool event(JZEvent *event) override;
    virtual void onNetEvent(JZNetResEvent *event) = 0;
    void wakeUpNet();

    void postReqEvent(JZNetReqEvent *event);
    uint64_t openConn(const JZSocketInfo &info, std::shared_ptr<JZNetReqEventData> data = std::shared_ptr<JZNetReqEventData>());
    void closeConn(uint64_t conn,bool closeImmediate);
    void writeConn(uint64_t conn, SocketWrite type, const void *buf, int len);
};

#endif // !JZ_NET_OBJECT_H_
