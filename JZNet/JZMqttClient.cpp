#include <string>
#include "JZMqttClient.h" 
#include "JZApplication.h"
#include "JZElapsedTimer.h"
#include "JZLog.h"
#include "JZPlatform.h"
#include "JZMqttServer.h"
#include "JZDebug.h"

using namespace std;

#define MQTT_CMD_PUBACK 4
#define MQTT_CMD_PUBCOMP 7

static const uint64_t QOS_RETRANSMIT_MS = 5000;   // 出站 QoS 未确认时的重传间隔

JZ_OBJECT_IMPL(JZMqttClient, JZNetObject)

JZMqttClient::JZMqttClient(JZObject* parent) 
    : JZNetObject(parent), sigConnect(this), sigDisconnect(this), sigMessage(this)
{
    m_client = INVALID_NET_HANDLE;
    m_state = UnconnectedState;
    m_server = nullptr;
    m_qosTimer = new JZTimer(this);
    connect(&m_qosTimer->sigTimeout, this, &JZMqttClient::onQosTimer);
}

JZMqttClient::~JZMqttClient()
{
    disconnectFromHost();
}

SocketState JZMqttClient::state() const
{
    return m_state;
}

void JZMqttClient::connectToHost(std::string url, const JZMqttConnectOpt& opt)
{
    JZ_ASSERT(!m_client);

    auto pkt = makeMgOptPacket(opt);

    JZSocketInfo info;
    info.type = JZSocketInfo::MqttClient;
    info.url = url;
    m_client = openConn(info, pkt);
    m_state = ConnectingState;
}

bool JZMqttClient::waitForConnected(int ms)
{
    JZElapsedTimer t;
    t.start();
    while (!t.hasExpired(ms))
    {
        if (m_state == ConnectedState)
            return true;
        else if (m_state == UnconnectedState)
            return false;

        jzApp->processEvents();
        JZThread::msleep(20);
    }
    return false;
}

void JZMqttClient::closeInternal(bool is_error)
{
    if (m_client != INVALID_NET_HANDLE)
    {
        clearOutboundQos();          // 连接断开, 在途 QoS 消息作废
        closeConn(m_client, is_error);
        m_client = INVALID_NET_HANDLE;
    }

    int pre_state = m_state;
    m_state = UnconnectedState;
    if (pre_state == ConnectedState)
        sigDisconnect.emit();
}

void JZMqttClient::disconnectFromHost()
{
    closeInternal(false);
}

void JZMqttClient::write(const std::shared_ptr<JZMgOptPacket> packet)
{
    JZNetReqEvent* req = new JZNetReqEvent();
    req->netCmd = JZNetReqEvent::Net_Write;
    req->writeCmd = WriteMqttMessage;
    req->conn = m_client;
    req->extData = packet;

    auto inst = JZNetThread::instance();
    jzApp->postEvent(inst, req);
    inst->wakeUpNet();
}

int JZMqttClient::publish(const JZMqttPublishOpt& opt)
{
    if (opt.qos > 0)
    {
        // 自管 packetId 并交给 mongoose(置 DUP 标志), 用于重传与确认跟踪
        uint16_t id = ++m_nextPacketId;
        if (id == 0) id = ++m_nextPacketId;     // 0 保留为无效

        JZMqttPublishOpt out = opt;
        out.retransmitId = (int)id;
        m_outboundQos[id] = {id, out, JZPlatform::getTickCountMs()};

        auto ptr = makeMgOptPacket(out);
        write(ptr);

        if (m_qosTimer && !m_qosTimer->isActive())
            m_qosTimer->start(1000);            // 启动重传扫描
        return (int)id;
    }

    auto ptr = makeMgOptPacket(opt);
    write(ptr);
    return 0;
}

void JZMqttClient::onQosTimer()
{
    uint64_t now = JZPlatform::getTickCountMs();
    for (auto& kv : m_outboundQos)
    {
        if (now - kv.second.sendTimeMs >= QOS_RETRANSMIT_MS)
        {
            kv.second.sendTimeMs = now;
            auto ptr = makeMgOptPacket(kv.second.opt);   // retransmitId 已设置 → DUP
            write(ptr);
        }
    }
    if (m_outboundQos.empty() && m_qosTimer)
        m_qosTimer->stop();
}

void JZMqttClient::onPubAck(uint16_t id)
{
    m_outboundQos.erase(id);
    if (m_outboundQos.empty() && m_qosTimer)
        m_qosTimer->stop();
}

void JZMqttClient::clearOutboundQos()
{
    m_outboundQos.clear();
    if (m_qosTimer)
        m_qosTimer->stop();
}

void JZMqttClient::subscribe(const JZMqttSubOpt& opt)
{
    auto ptr = makeMgOptPacket(true, opt);
    write(ptr);
}

void JZMqttClient::unsubscribe(const JZMqttSubOpt& opt)
{
    auto ptr = makeMgOptPacket(false, opt);
    write(ptr);
}


void JZMqttClient::onNetEvent(JZNetResEvent* event)
{
    if (m_server)
    {
        m_server->onClinetNetEvent(this, event);
        return;
    }

    switch (event->event)
    {
    case JZ_NET_EV_CLOSE:
        if (m_client != INVALID_NET_HANDLE)
        {
            m_client = INVALID_NET_HANDLE;
            closeInternal(false);
        }
        break;
    case JZ_NET_EV_MQTT_OPEN:
        m_state = ConnectedState;
        sigConnect.emit();
        break;
    case JZ_NET_EV_MQTT_CMD:
    {
        // 独立 client 角色: 处理出站 QoS 的确认
        JZMqttResPacket *packet = (JZMqttResPacket*)(event->extData.get());
        JZMqttMessage* msg = &packet->message;
        if (msg->cmd == MQTT_CMD_PUBACK || msg->cmd == MQTT_CMD_PUBCOMP)
            onPubAck(msg->id);
        break;
    }
    case JZ_NET_EV_MQTT_MSG:
    {
        JZMqttResPacket *packet = (JZMqttResPacket*)(event->extData.get());
        JZMqttMessage msg = std::move(packet->message);
        sigMessage.emit(msg);
        break;
    }
    default:
        break;
    }
}