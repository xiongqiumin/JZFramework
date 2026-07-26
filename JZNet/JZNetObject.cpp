#include "mongoose.h"
#include "JZNetObject.h"
#include "JZTx.h"
#include "JZApplication.h"
#include "JZStd.h"
#include "JZElapsedTimer.h"
#include "JZDebug.h"
#include "JZPlatform.h"
#include "JZWebSocket.h"
#include "JZNetworkReply.h"
#include "JZUdpSocket.h"
#include "JZUrl.h"
#include "JZScopeGuard.h"
#include "JZMqttPacket.h"
#include "JZTx.h"

int JZNET_MAX_RECV = 3 * 1024 * 1024;
int JZNET_MAX_SEND = 3 * 1024 * 1024;

JZNetEvent mgEventToNetEvent(int ev)
{
    switch (ev)
    {
        case MG_EV_ERROR:
            return JZ_NET_EV_ERROR;
        case MG_EV_OPEN:
            return JZ_NET_EV_OPEN;
        case MG_EV_POLL:
            return JZ_NET_EV_POLL;
        case MG_EV_RESOLVE:
            return JZ_NET_EV_RESOLVE;
        case MG_EV_CONNECT:
            return JZ_NET_EV_CONNECT;
        case MG_EV_ACCEPT:
            return JZ_NET_EV_ACCEPT;
        case MG_EV_TLS_HS:
            return JZ_NET_EV_TLS_HS;
        case MG_EV_READ:
            return JZ_NET_EV_READ;
        case MG_EV_WRITE:
            return JZ_NET_EV_WRITE;
        case MG_EV_CLOSE:
            return JZ_NET_EV_CLOSE;
        case MG_EV_HTTP_HDRS:
            return JZ_NET_EV_HTTP_HDRS;
        case MG_EV_HTTP_MSG:
            return JZ_NET_EV_HTTP_MSG;
        case MG_EV_WS_OPEN:
            return JZ_NET_EV_WS_OPEN;
        case MG_EV_WS_MSG:
            return JZ_NET_EV_WS_MSG;
        case MG_EV_WS_CTL:
            return JZ_NET_EV_WS_CTL;
        case MG_EV_MQTT_CMD:
            return JZ_NET_EV_MQTT_CMD;
        case MG_EV_MQTT_MSG:
            return JZ_NET_EV_MQTT_MSG;
        case MG_EV_MQTT_OPEN:
            return JZ_NET_EV_MQTT_OPEN;
        case MG_EV_SNTP_TIME:
            return JZ_NET_EV_SNTP_TIME;
        default:
            JZ_UNREACHABLE();
            return JZ_NET_EV_ERROR;
    }
}

const char *netEventString(int event)
{
    switch (event)
    {
        case JZ_NET_EV_ERROR:
            return "JZ_NET_EV_ERROR";
        case JZ_NET_EV_OPEN:
            return "JZ_NET_EV_OPEN";
        case JZ_NET_EV_POLL:
            return "JZ_NET_EV_POLL";
        case JZ_NET_EV_RESOLVE:
            return "JZ_NET_EV_RESOLVE";
        case JZ_NET_EV_CONNECT:
            return "JZ_NET_EV_CONNECT";
        case JZ_NET_EV_ACCEPT:
            return "JZ_NET_EV_ACCEPT";
        case JZ_NET_EV_TLS_HS:
            return "JZ_NET_EV_TLS_HS";
        case JZ_NET_EV_READ:
            return "JZ_NET_EV_READ";
        case JZ_NET_EV_WRITE:
            return "JZ_NET_EV_WRITE";
        case JZ_NET_EV_CLOSE:
            return "JZ_NET_EV_CLOSE";
        case JZ_NET_EV_HTTP_HDRS:
            return "JZ_NET_EV_HTTP_HDRS";
        case JZ_NET_EV_HTTP_MSG:
            return "JZ_NET_EV_HTTP_MSG";
        case JZ_NET_EV_WS_OPEN:
            return "JZ_NET_EV_WS_OPEN";
        case JZ_NET_EV_WS_MSG:
            return "JZ_NET_EV_WS_MSG";
        case JZ_NET_EV_WS_CTL:
            return "JZ_NET_EV_WS_CTL";
        case JZ_NET_EV_MQTT_CMD:
            return "JZ_NET_EV_MQTT_CMD";
        case JZ_NET_EV_MQTT_MSG:
            return "JZ_NET_EV_MQTT_MSG";
        case JZ_NET_EV_MQTT_OPEN:
            return "JZ_NET_EV_MQTT_OPEN";
        case JZ_NET_EV_SNTP_TIME:
            return "JZ_NET_EV_SNTP_TIME";
        default:
            return "UNKNOWN_JZ_NET_EVENT";
    }
}

static std::string getHttpCodeText(const char *bufr, int len)
{
    const char *end = bufr + len;
    const char *p = bufr;

    // 查找 HTTP/ 前缀（安全不越界）
    while (p <= end - 5 && memcmp(p, "HTTP/", 5) != 0)
    {
        ++p;
    }
    if (p > end - 5)
    {
        return "";
    }

    // 跳过协议版本 HTTP/1.x
    while (p < end && *p != ' ')
        ++p;
    while (p < end && *p == ' ')
        ++p;

    // 跳过状态码数字
    while (p < end && isdigit(static_cast<unsigned char>(*p)))
        ++p;
    while (p < end && *p == ' ')
        ++p;

    // 提取描述文本到行尾
    const char *start = p;
    while (p < end && *p != '\r' && *p != '\n')
        ++p;

    // 无有效文本 → 返回空
    if (start == p)
    {
        return "";
    }

    // 正常返回文本
    return std::string(start, p - start);
}

//JZNetOpenReq
JZNetOpenReq::JZNetOpenReq()
{
    object = nullptr;
    conn = INVALID_NET_HANDLE;
}

//JZNetReqEventData
JZNetReqEventData::~JZNetReqEventData()
{
}

//JZNetReqEvent
JZNetReqEvent::JZNetReqEvent()
    :JZEvent(JZEvent_netReq)
{
    netCmd = 0;
    openReq = nullptr;
    sem = nullptr;
    conn = 0;
    writeCmd = 0;
}

JZNetReqEvent::~JZNetReqEvent()
{
}

//JZNetResEventData
JZNetResEventData::~JZNetResEventData()
{
}

//JZNetEvent
JZNetResEvent::JZNetResEvent()
    :JZEvent(JZEvent_netRes)
{
    conn = 0;
    event = JZ_NET_EV_ERROR;
}

JZNetResEvent::~JZNetResEvent()
{
}

//JZNetThread
void mg_thread_fn(mg_connection *c, int ev, void *ev_data)
{
    JZNetThread *inst = JZNetThread::instance();
    if (ev == MG_EV_POLL || ev == MG_EV_WAKEUP || ev == MG_EV_RESOLVE)
        return;
    else if (ev == MG_EV_OPEN)
    {
        JZ_ASSERT(!map_contains(inst->m_connMap, (uint64_t)c->id));
        
        JZNetThread::ConnInfo conn_info;
        conn_info.c = c;
        map_insert(inst->m_connMap, (uint64_t)c->id, conn_info);
        return;
    }

    JZNetResEvent *event = new JZNetResEvent();
    event->conn = c->id;
    event->event = mgEventToNetEvent(ev);

    uint64_t accept_id = INVALID_NET_HANDLE;
    if (ev == MG_EV_READ)
    {
        auto it = inst->m_connMap.find(c->id);
        if (it->second.type == JZSocketInfo::TcpSocket)
        {
            event->binData = JZByteArray((char*)c->recv.buf, (int)c->recv.len);
            mg_iobuf_del(&c->recv, 0, c->recv.len);
        }
        else if (it->second.type == JZSocketInfo::UdpSocket ||
                 it->second.type == JZSocketInfo::UdpListen)
        {
            event->binData = JZByteArray((char*)c->recv.buf, (int)c->recv.len);
            mg_iobuf_del(&c->recv, 0, c->recv.len);
            auto ptr = std::make_shared<JZUdpResData>();
            char *ip_str = mg_mprintf("%M", mg_print_ip, &c->rem);
            ptr->peerIp = std::string(ip_str);
            mg_free(ip_str);
            ptr->peerPort = mg_ntohs(c->rem.port);
            event->extData = ptr;
        }
    }
    else if (ev == MG_EV_WRITE)
    {        
        long *bytes_written = (long*)ev_data;
        JZDataStream s(&event->binData, JZ::WriteOnly);
        s << (int32_t)(*bytes_written);
    }
    else if (ev == MG_EV_ACCEPT)
    {
        //这里对 
        mg_connection* lsn = (mg_connection*)ev_data;
        auto listen_it = inst->m_connMap.find(lsn->id);
        
        auto conn_it = inst->m_connMap.find(c->id);
        conn_it->second.acceptId = listen_it->first;

        if (listen_it->second.type == JZSocketInfo::TcpServer)
        { 
            conn_it->second.type = JZSocketInfo::TcpSocket;
            accept_id = listen_it->first;
        }
        else if (listen_it->second.type == JZSocketInfo::HttpServer)
        {
            conn_it->second.type = JZSocketInfo::HttpServerClient;
            accept_id = listen_it->first;
        }
        else if (listen_it->second.type == JZSocketInfo::WebSocketListen)
        {
            conn_it->second.type = JZSocketInfo::WebServerClient;
        }
        else if (listen_it->second.type == JZSocketInfo::MqttServer)
        {
            conn_it->second.type = JZSocketInfo::MqttServerClient;
            accept_id = listen_it->first;
        }
        else
        {
            JZ_UNREACHABLE();
        }
    }
    else if (ev == MG_EV_HTTP_MSG)
    {
        mg_http_message* hm = (mg_http_message*)ev_data;

        auto conn_it = inst->m_connMap.find(c->id);
        if (conn_it->second.type == JZSocketInfo::WebServerClient)
        {
            auto type = mg_http_get_header(hm, "Upgrade");
            if (type && std::string(type->buf, type->len) == "websocket")
            {
                mg_ws_upgrade(c, hm, "");
                return;
            }
        }

        auto http_ptr = std::make_shared<JZHttpResData>();
        http_ptr->httpCode = mg_http_status(hm);
        if (conn_it->second.type == JZSocketInfo::HttpClient)
            http_ptr->httpCodeText = getHttpCodeText(hm->head.buf, (int)hm->head.len);
        event->extData = http_ptr;
        event->binData = JZByteArray(hm->message.buf, (int)hm->message.len);
    }
    else if (ev == MG_EV_WS_OPEN)
    {
        auto it = inst->m_connMap.find(c->id);
        if (it->second.type == JZSocketInfo::WebServerClient)
        {
            accept_id = it->second.acceptId;
        }
    }
    else if (ev == MG_EV_WS_MSG)
    {
        mg_ws_message *ws_msg = (mg_ws_message*)ev_data;
        auto ws_ptr = std::make_shared<JZWsResData>();
        ws_ptr->wsFlag = ws_msg->flags;
        event->extData = ws_ptr;
        event->binData = JZByteArray(ws_msg->data.buf, (int)ws_msg->data.len);            
    }
    else if (ev == MG_EV_MQTT_OPEN)
    {
        
    }
    else if (ev == JZ_NET_EV_MQTT_CMD)
    {
        mg_mqtt_message *mg_msg = (mg_mqtt_message *)ev_data;

        std::shared_ptr<JZMqttResPacket> ptr = std::make_shared<JZMqttResPacket>();
        mgMsgToMqttMessage(mg_msg, &ptr->message);
        event->extData = ptr;
    }
    else if (ev == JZ_NET_EV_MQTT_MSG)
    {
        mg_mqtt_message *mg_msg = (mg_mqtt_message *)ev_data;

        std::shared_ptr<JZMqttResPacket> ptr = std::make_shared<JZMqttResPacket>();
        mgMsgToMqttMessage(mg_msg, &ptr->message);
        event->extData = ptr;
    }
    else if (ev == MG_EV_ERROR)
    {
        const char *err_msg = (const char*)ev_data;
        event->binData = str_toUtf8(err_msg);
    }

    JZMutexLocker locker(&inst->m_objectMutex);
    if (accept_id != INVALID_NET_HANDLE)
    {
        auto accept_it = inst->m_connMap.find(accept_id);
        JZNetObject* accept_object = nullptr;
        if (accept_it != inst->m_connMap.end())
            accept_object = (JZNetObject*)accept_it->second.object.object();
            
        if (accept_object && vec_contains(inst->m_objectList, accept_object))
        {
            JZNetObject *bind_object = accept_object->onAccept(event->conn);
            if (bind_object)
                inst->bindObject(event->conn, bind_object);
        }
        delete event;
        return;
    }

    JZNetObject *object = (JZNetObject*)inst->m_connMap[c->id].object.object();
    if (!object || !vec_contains(inst->m_objectList, object))
    {
        delete event;
    }
    else
    {
        jzApp->postEvent(object, event);
    }

    if (ev == MG_EV_CLOSE)
    {
        map_remove(inst->m_connMap, (uint64_t)c->id);
    }
}

JZNetThread::ConnInfo::ConnInfo()
{
    type = JZSocketInfo::None;
    c = nullptr;
    acceptId = INVALID_NET_HANDLE;
}

//JZNetThread
static JZNetThread *g_netThread = nullptr;
JZNetThread *JZNetThread::instance()
{
    if (!g_netThread)
    {        
        g_netThread = new JZNetThread(jzApp);
    }

    return g_netThread;
}

JZNetThread::JZNetThread(JZObject *parent)
    :JZThread(parent)
{
    JZThreadConfig cfg;
    cfg.stackSize = 8 * 1024;
    cfg.priority = 5;

    connect(&sigFinished, this, &JZNetThread::onFinished);
    moveToThread(this);
    setObjectName("JZNetThread");
    setConfig(cfg);

    mg_log_set(MG_LL_ERROR);
    m_mgr = new mg_mgr();
    mg_mgr_init(m_mgr);
    m_mgr->dns4.url = "udp://223.5.5.5:53";    // 阿里云 IPv4 DNS
    m_mgr->dns6.url = "udp://[2400:3200::1]:53";  // 阿里云 IPv6 DNS

    mg_wakeup_init(m_mgr);
}

JZNetThread::~JZNetThread()
{
    quit();
    wait();

    mg_mgr_free(m_mgr);
    delete m_mgr;
    m_mgr = nullptr;
   
    g_netThread = nullptr;
}

void JZNetThread::onFinished()
{
    moveToThread(m_parent->thread());
}

void JZNetThread::run()
{
    while (m_running)
    {
        mg_mgr_poll(m_mgr, 20);
        processEvents();
    }
}

bool JZNetThread::event(JZEvent* event)
{
    if (event->type() == JZEvent_netReq)
    {
        JZNetReqEvent* net_event = (JZNetReqEvent*)event;
        onNetEvent(net_event);
    }

    return JZObject::event(event);
}

void JZNetThread::initTls(mg_connection *conn, const std::string &url_str) { 
    JZUrl url(url_str);
    std::string host = url.host();

    mg_tls_opts opts;
    memset(&opts, 0, sizeof(mg_tls_opts));
    opts.skip_verification = 1;
    opts.name.buf = (char*)host.c_str();
    opts.name.len = host.size();

    mg_tls_init(conn, &opts);
}

void JZNetThread::onNetEvent(JZNetReqEvent* event)
{
    JZScopeGuard sem_guard([event] { 
            if (event->sem)
                event->sem->release();
        });

    if (event->netCmd == JZNetReqEvent::Net_Open)
    {
        JZNetOpenReq* req = event->openReq;
        auto& info = req->info;
        auto object = req->object;

        mg_connection *conn = nullptr;    
        if (info.type == JZSocketInfo::TcpSocket)
            conn = mg_connect(m_mgr, info.url.c_str(), mg_thread_fn, nullptr);
        else if (info.type == JZSocketInfo::TcpServer)
            conn = mg_listen(m_mgr, info.url.c_str(), mg_thread_fn, nullptr);
        else if (info.type == JZSocketInfo::UdpSocket)
            conn = mg_connect(m_mgr, info.url.c_str(), mg_thread_fn, nullptr);
        else if (info.type == JZSocketInfo::UdpListen)
            conn = mg_listen(m_mgr, info.url.c_str(), mg_thread_fn, nullptr);
        else if (info.type == JZSocketInfo::HttpClient)
        {
            conn = mg_http_connect(m_mgr, info.url.c_str(), mg_thread_fn, nullptr);
            if (str_startsWith(info.url, "https://")) {
                initTls(conn, info.url);
            }
        }
        else if (info.type == JZSocketInfo::HttpServer)
            conn = mg_http_listen(m_mgr, info.url.c_str(), mg_thread_fn, nullptr);
        else if (info.type == JZSocketInfo::WebSocket)
        {
            conn = mg_ws_connect(m_mgr, info.url.c_str(), mg_thread_fn, nullptr, "%s", info.websocketExt.c_str());
            if (str_startsWith(info.url, "wss://")) {
                initTls(conn, info.url);
            }
        }
        else if (info.type == JZSocketInfo::WebSocketListen)
        {
            conn = mg_http_listen(m_mgr, info.url.c_str(), mg_thread_fn, nullptr);
        }
        else if (info.type == JZSocketInfo::MqttClient)
        {
            JZMgOptPacket *pkt = (JZMgOptPacket *)(event->extData.get());
            conn = mg_mqtt_connect(m_mgr, info.url.c_str(), pkt->opt, mg_thread_fn, nullptr);
        }
        else if (info.type == JZSocketInfo::MqttServer)
        {
            conn = mg_mqtt_listen(m_mgr, info.url.c_str(), mg_thread_fn, nullptr);
        }
        else
        {
            JZ_UNREACHABLE();
        }

        if (conn)
        {
            req->conn = conn->id;

            ConnInfo conn_info;
            conn_info.type = info.type;
            conn_info.object = object->guardPointer();
            conn_info.c = conn;
            m_connMap[conn->id] = conn_info;
        }
    }
    else
    {
        //多线程, 可能已经不存在了
        auto conn_it = m_connMap.find(event->conn);
        if (conn_it == m_connMap.end())
        {
            return;
        }

        auto c = conn_it->second.c;
        if (event->netCmd == JZNetReqEvent::Net_Draining)
        {
            c->is_draining = 1;
        }
        else if(event->netCmd == JZNetReqEvent::Net_Close)
        {
            c->is_closing = 1;
        }
        else if (event->netCmd == JZNetReqEvent::Net_Write)
        {
            if(c->is_draining || c->is_closing)
                return;

            const char* buf = event->buffer.data();
            int len = event->buffer.size();

            bool send_finished = true;
            if (event->writeCmd == WriteBin)
            {
                // 防止 send buffer 无限膨胀导致 OOM
                if(c->send.len + len > JZNET_MAX_SEND)
                {
                    send_finished = false;
                    goto send_end;
                }
                send_finished = mg_send(c, buf, len);
            }
            else if (event->writeCmd == WriteWebSocketText)
            {
                if(c->send.len + len > JZNET_MAX_SEND)
                {
                    send_finished = false;
                    goto send_end;
                }
                send_finished = (mg_ws_send(c, buf, len, WEBSOCKET_OP_TEXT) >= len);
            }
            else if (event->writeCmd == WriteWebSocketBin)
            {
                if(c->send.len + len > JZNET_MAX_SEND)
                {
                    send_finished = false;
                    goto send_end;
                }
                send_finished = (mg_ws_send(c, buf, len, WEBSOCKET_OP_BINARY) >= len);
            }
            else if (event->writeCmd == WriteMqttMessage)
            {
                if(c->send.len > JZNET_MAX_SEND)
                {
                    send_finished = false;
                    goto send_end;
                }

                //mqtt 不需要检测， 发送失败会自动断开
                JZMgOptPacket *pkt = (JZMgOptPacket *)(event->extData.get());
                if (pkt->command == JZMgOptPacket::Publish)
                    mg_mqtt_pub(c, pkt->opt);
                else if (pkt->command == JZMgOptPacket::Subscribe)
                    mg_mqtt_sub(c, pkt->opt);
                else if (pkt->command == JZMgOptPacket::Unsubscribe)
                    mg_mqtt_unsub(c, pkt->opt);
                else
                {
                    JZ_UNREACHABLE();
                }
            }
            else if (event->writeCmd == WriteMqttServerResp)
            {
                JZMgServerPacket *pkt = (JZMgServerPacket *)(event->extData.get());
                mg_mqtt_send_header(c, pkt->cmd, pkt->flags, pkt->len);
                if (pkt->buffer.size() > 0)
                {
                    if(c->send.len + pkt->buffer.size() > JZNET_MAX_SEND)
                    {
                        c->is_draining = 1;
                        return;
                    }
                    send_finished = mg_send(c, pkt->buffer.data(), pkt->buffer.size());
                }
            }
            else if (event->writeCmd == WriteUdp)
            {
                //udp 也不检测失败（UDP 不走 iobuf，无需限制）
                JZUdpWriteData *udp_data = (JZUdpWriteData *)(event->extData.get());
                if (udp_data && !udp_data->ip.empty())
                {
                    mg_addr addr;
                    memset(&addr, 0, sizeof(addr));
                    if (mg_aton(mg_str(udp_data->ip.c_str()), &addr))
                    {
                        addr.port = mg_htons((uint16_t)udp_data->port);
                        c->rem = addr;
                    }
                }
                mg_send(c, buf, len);
            }

send_end:            
            if(!send_finished)
            {
                jzWarning() << "send faild, net draining";
                c->is_draining = 1;
            }
        }
        else if (event->netCmd == JZNetReqEvent::Net_HttpReplyFinish)
        {
            c->is_resp = 0;
        }
        else
        {   
            JZ_UNREACHABLE();
        }
    }
}

void JZNetThread::wakeUpNet()
{
    mg_wakeup(m_mgr, 1, nullptr, 0);
}

void JZNetThread::addObject(JZNetObject *object)
{
    bool need_lock = JZThread::currentThreadId() != this->id();
    if (need_lock)
    {
        JZMutexLocker locker(&m_objectMutex);
        m_objectList.push_back(object);
        if (m_objectList.size() == 1 && !isRunning())
            start();
    }
    else
    {
        //在 accpet 时间中创建的 object 会走到这里
        m_objectList.push_back(object);
    }
}

void JZNetThread::removeObject(JZNetObject *object)
{
    JZMutexLocker locker(&m_objectMutex);
    if(m_objectList.size() == 0)
        return;

    vec_removeAll(m_objectList, object);
    for(auto &it : m_connMap)
    {
        if (it.second.object.object() == object) 
        {
            it.second.object = JZObjectPointer();
            break;
        }
    }
}

void JZNetThread::bindObject(uint64_t conn, JZNetObject* object)
{
    auto it = m_connMap.find(conn);
    it->second.object = object->guardPointer();
}

//JZNetObject
JZ_OBJECT_IMPL(JZNetObject, JZObject)

JZNetObject::JZNetObject(JZObject *parent)
    :JZObject(parent)
{
    JZNetThread::instance()->addObject(this);
}

JZNetObject::~JZNetObject()
{
    removeNetObject();
}

void JZNetObject::removeNetObject()
{
    JZNetThread::instance()->removeObject(this);
}   

bool JZNetObject::event(JZEvent *event)
{
    JZTX_FUNC_TRACE
    if (event->type() == JZEvent_netRes) 
    {
        JZNetResEvent *net_event = (JZNetResEvent*)event;
        onNetEvent(net_event);
    }

    return JZObject::event(event);
}

JZNetObject *JZNetObject::onAccept(uint64_t conn)
{
    return nullptr;
}

void JZNetObject::wakeUpNet()
{
    JZNetThread::instance()->wakeUpNet();
}

void JZNetObject::postReqEvent(JZNetReqEvent *req)
{
    auto inst = JZNetThread::instance();
    jzApp->postEvent(inst, req);
    inst->wakeUpNet();
}

uint64_t JZNetObject::openConn(const JZSocketInfo &info, std::shared_ptr<JZNetReqEventData> data)
{
    JZSemaphore sem;

    JZNetOpenReq result;
    result.object = this;
    result.info = info;

    JZNetReqEvent* req = new JZNetReqEvent();
    req->netCmd = JZNetReqEvent::Net_Open;
    req->openReq = &result;
    req->sem = &sem;
    req->extData = data;

    auto inst = JZNetThread::instance();
    jzApp->postEvent(inst, req);
    inst->wakeUpNet();
    sem.acquire();

    return result.conn;
}

void JZNetObject::closeConn(uint64_t conn, bool isError)
{    
    JZ_ASSERT(conn != INVALID_NET_HANDLE);

    JZNetReqEvent* req = new JZNetReqEvent();
    if(!isError)
        req->netCmd = JZNetReqEvent::Net_Draining;
    else
        req->netCmd = JZNetReqEvent::Net_Close;

    req->conn = conn;

    auto inst = JZNetThread::instance();
    jzApp->postEvent(inst, req);
    inst->wakeUpNet();
}

void JZNetObject::writeConn(uint64_t conn, SocketWrite type, const void *buf, int len)
{
    JZNetReqEvent* req = new JZNetReqEvent();
    req->netCmd = JZNetReqEvent::Net_Write;
    req->writeCmd = type;
    req->conn = conn;
    req->buffer = JZByteArray((const char*)buf, len);

    auto inst = JZNetThread::instance();
    jzApp->postEvent(inst, req);
    inst->wakeUpNet();
}

//JZNet
void JZNet::setConfig(JZNetConfig config)
{
    JZNET_MAX_RECV = config.maxRecvSize;
    JZNET_MAX_SEND = config.maxSendSize;
}