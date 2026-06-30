#include <string>
#include "JZMqttClient.h" 
#include "JZApplication.h"
#include "JZElapsedTimer.h"
#include "JZLog.h"
#include "JZPlatform.h"
#include "JZMqttServer.h"
#include "JZDebug.h"

using namespace std;

JZ_OBJECT_IMPL(JZMqttClient, JZNetObject)

JZMqttClient::JZMqttClient(JZObject* parent) 
    : JZNetObject(parent), sigConnect(this), sigDisconnect(this), sigMessage(this)
{
    m_client = INVALID_NET_HANDLE;
    m_state = UnconnectedState;
    m_server = nullptr;
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
    auto ptr = makeMgOptPacket(opt);
    write(ptr);
    return 0;
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

    int ev = event->event;
    if (ev == JZ_NET_EV_CONNECT)
    {
    }
    else if (ev == JZ_NET_EV_CLOSE)
    {
        if (m_client != INVALID_NET_HANDLE)
        {
            m_client = INVALID_NET_HANDLE;
            closeInternal(false);
        }
    }
    else if (ev == JZ_NET_EV_READ)
    {
    }
    else if (ev == JZ_NET_EV_ERROR)
    {
    }
    else if (ev == JZ_NET_EV_MQTT_OPEN)
    {
        m_state = ConnectedState;
        sigConnect.emit();
    }
    else if (ev == JZ_NET_EV_MQTT_CMD)
    {
    }
    else if (ev == JZ_NET_EV_MQTT_MSG)
    {
        JZMqttResPacket *packet = (JZMqttResPacket*)(event->extData.get());
        JZMqttMessage msg = std::move(packet->message);
        sigMessage.emit(msg);
    }
}